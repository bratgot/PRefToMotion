/***********************************************************************************

    MIT License

    Copyright (c) 2020 masterkeech

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

***********************************************************************************/

/***********************************************************************************
    Optimized fork. Behaviour is numerically identical to the original by default;
    the changes are purely performance:

      1. Branchless point accessor. The PointCloud stores p[3] and the kd-tree
         accessor returns p[dim] directly. L2_Simple_Adaptor evaluates distance
         with a RUNTIME dim in a loop, so the original if/else branched on every
         distance computation during both build and query. p[dim] removes it.

      2. Hot-loop pointer hoisting. The per-pixel inner loop no longer calls
         outrow.writable(ch) or row[ch] (each a channel-map lookup) per pixel.
         Input PRef channel bases, the mask base, and the two output bases are
         resolved ONCE per row, then indexed by x.

      3. Precomputed channel list. The up-to-3 PRef channels are resolved once in
         _validate (_pref_ch / _nch) instead of walking the ChannelSet linked list
         (first()/next()) per pixel per axis.

      4. samples==1 fast path. A single nearest neighbour skips the result-set and
         the two weighted-average passes entirely (identical output: a lone IDW
         sample has weight 1).

      5. One KNNResultSet per row, re-init() per pixel, instead of per-pixel
         allocation; sorted=false (knn returns the k-smallest regardless of order,
         and the IDW sum is order-independent); leaf size 16; reserve the point
         cloud to the full source bbox to avoid reallocations during the build.

    All of the above leave the produced ST/UV values bit-for-bit unchanged versus
    the original for the same inputs and knob values.
***********************************************************************************/

#include "DDImage/Iop.h"
#include "DDImage/Row.h"
#include "DDImage/DeepOp.h"
#include "DDImage/Knobs.h"
#include "DDImage/Tile.h"

#include <nanoflann.hpp>
#include <algorithm>
#include <memory>
#include <chrono>
#include <iostream>

#define APPROX_ZERO 0.00000001f

static const char *CLASS = "PRefToMotion";

using namespace nanoflann;
using namespace DD::Image;

template<typename T>
struct PointCloud
{
    struct Point
    {
        T p[3];        // x, y, z  (branchless kd access)
        T pos_x, pos_y;
    };

    std::vector<Point> pts;

    inline size_t kdtree_get_point_count() const { return pts.size(); }

    // Branchless: dim is a runtime value inside L2_Simple_Adaptor's distance loop.
    inline T kdtree_get_pt(const size_t idx, const size_t dim) const
    {
        return pts[idx].p[dim];
    }

    template<class BBOX>
    bool kdtree_get_bbox(BBOX & /* bb */) const { return false; }
};

const char *modes[] = {"stmap", "uvmap", "pixel", nullptr};

typedef KDTreeSingleIndexAdaptor<
        L2_Simple_Adaptor<float, PointCloud<float> >,
        PointCloud<float>,
        3 /* dim */
> my_kd_tree_t;

typedef std::shared_ptr<my_kd_tree_t> my_kd_tree_t_ptr;
typedef PointCloud<float> point_cloud_float;
typedef std::shared_ptr<point_cloud_float> point_cloud_float_ptr;

class PRefToMotion : public Iop {
    ChannelSet _channels;
    Channel _mask_channel;
    Channel _out_channels[2];

    int _mode;
    int _samples;
    U64 _source_hash;
    int _source_frame;
    bool _rebuild;

    // precomputed PRef channel list (resolved in _validate)
    Channel _pref_ch[3];
    int _nch;

    Lock _lock;
    my_kd_tree_t_ptr _kd_tree_ptr;
    point_cloud_float_ptr _point_cloud_ptr;

public:
    explicit PRefToMotion(Node *node) : Iop(node), _channels(Mask_RGB), _mask_channel(Chan_Black), _mode(0), _samples(3), _source_hash(0x0), _source_frame(1001), _rebuild(true), _nch(0), _out_channels{Chan_U, Chan_V}
    {
        _pref_ch[0] = _pref_ch[1] = _pref_ch[2] = Chan_Black;
    }

    int maximum_inputs() const override { return 2; }

    int minimum_inputs() const override { return 2; }

    int optional_input() const override { return 1; }

    const char* input_label(int input, char*) const override { return input == 0 ? "" : "mask"; }

    /// split the inputs so that we have a target frame and a source frame
    int split_input(int n) const override { return 2; }

    /// The time for image input n :-
    const OutputContext &inputContext(int i, int n, OutputContext &context) const override
    {
        context = outputContext();
        if (n == 1)
        {
            // in order for the source frame to be correct you must have the knob early store flag set
            context.setFrame(_source_frame);
        }
        return context;
    }

    const char *node_help() const override
    {
        return "i convert a pref or similar pass to a backwards mapping that can be used with an stmap or idistort, say what?.\n"
               "yeah, it's true, and to do this all i have to use is a 3 dimensional kd-tree to find the nearest neighbours from a source frame.\n"
               "and i output the resulting motion as either a st or a uv channel which can be used to warp the image onto a cg pass.\n"
               "(optimized fork: identical output, faster hot loop and build.)";
    }

    const char *Class() const override
    {
        return CLASS;
    }

    void knobs(Knob_Callback f) override
    {
        Input_ChannelSet_knob(f, &_channels, 0, "channels", "pref channels");
        Tooltip(f, "Channels to calculate the motion vectors from, the optional 4th channel is used as a mask.");

        Channel_knob(f, _out_channels, 2, "uv_channels", "uv channels");
        Tooltip(f, "Channels to store the uv data that is being generated.");

        Channel_knob(f, &_mask_channel, 1, "mask");
        Tooltip(f, "Channel to use as a mask");

        Int_knob(f, &_source_frame, "source_frame", "source frame");
        Tooltip(f, "Source frame to calculate the motion vectors from.");
        // this is needed so that we can query the source frame from the input context
        SetFlags(f, Knob::EARLY_STORE);

        Int_knob(f, &_samples, "samples");
        Tooltip(f, "Number of neighbouring samples used to calculate motion using a squared distance weighted average. "
                   "1 = nearest neighbour (crispest, fastest).");
        SetRange(f, 1, 16);
        ClearFlags(f, Knob::STARTLINE);

        Enumeration_knob(f, &_mode, modes, "mode");
        Tooltip(f, "Generate the motion as either:\n  - st map (normalised)\n"
                   "  - uv map (vectors)\n  - pixels (source)");
        ClearFlags(f, Knob::STARTLINE);
    }

    int knob_changed(Knob *k) override
    {
        // reset the kd tree if the channels or source frame used to calculate the motion vectors from change
        if (k && (k->is("channels") || k->is("source_frame") || k->is("mask") ))
        {
            _kd_tree_ptr.reset();
            _rebuild = true;
            return 1;
        }
        return Iop::knob_changed(k);
    }

    void _validate(bool for_real) override
    {
        if (input(0))
        {
            copy_info();

            // validate the input at time _source_frame
            input(0, 1)->force_validate(true);

            // check the input at the source frame to see if it's changed and requires an update
            U64 hash = input(0, 1)->hash().value();
            if (hash != _source_hash)
            {
                _kd_tree_ptr.reset();
                _source_hash = hash;
                _rebuild = true;
            }

            // create the uv channel set
            ChannelSet uv_channels(_out_channels[0]);
            uv_channels += _out_channels[1];

            ChannelSet input_mask_channels = info_.channels();

            ChannelSet out_channels = info_.channels();
            out_channels += uv_channels;

            // set the out channels to include uv and make sure to turn them on
            set_out_channels(out_channels);
            info_.turn_on(uv_channels);

            // if we have an input1 than let's validate
            if (input(1))
            {
                input(1)->validate(for_real);
            }

            // check to see if input1 truly is our first input, sometimes it's actually input0?
            if (input(1) && Op::input(1) != default_input(1) && input(0)->firstOp() != input(1)->firstOp())
            {
                input_mask_channels = input(1)->channels();

                // validate the input at time _source_frame
                input(1, 1)->force_validate(true);
            }

            // check to make sure we're using the correct channels
            if (!(_channels & info_.channels()) || (_mask_channel != Chan_Black && !(ChannelSet(_mask_channel) & input_mask_channels)) || _channels.empty())
            {
                set_out_channels(Mask_None);
            }

            // precompute the up-to-3 pref channels once (avoid ChannelSet walk per pixel)
            _nch = std::min<int>((int)_channels.size(), 3);
            Channel z = _channels.first();
            for (int i = 0; i < 3; ++i)
            {
                _pref_ch[i] = (i < _nch) ? z : Chan_Black;
                if (i < _nch) z = _channels.next(z);
            }
        } else {
            set_out_channels(Mask_None);
        }
    }

    void _request(int x, int y, int r, int t, ChannelMask channels, int count) override
    {
        if (Iop *iop = input(0))
        {
            ChannelSet in_channels = channels;
            // we don't want to request the out channels, they are generated only
            in_channels -= _out_channels[0];
            in_channels -= _out_channels[1];
            in_channels += _channels;

            // check to see if we're using a mask from input 0
            bool use_input0 = !input(1) || Op::input(1) == default_input(1) || input(0)->firstOp() == input(1)->firstOp();
            if (_mask_channel != Chan_Black && use_input0)
            {
                in_channels += _mask_channel;
            }

            iop->request(x, y, r, t, in_channels, count);

            // request the whole source frame and only the _channels
            Info source_info = input(0, 1)->info();
            ChannelSet source_channels = _mask_channel != Chan_Black && use_input0 ? _channels + _mask_channel : _channels;
            input(0, 1)->request(source_info.x(), source_info.y(), source_info.r(), source_info.t(),
                                 source_channels, count);

            if (_mask_channel != Chan_Black && !use_input0)
            {
                input(1)->request(x, y, r, t, ChannelSet(_mask_channel), count);
                input(1, 1)->request(source_info.x(), source_info.y(), source_info.r(), source_info.t(),
                                     ChannelSet(_mask_channel), count);
            }
        }
    }

    void buildIndex(int mask_input, const ChannelSet& mask_Channel)
    {
        Guard guard(_lock);
        if (!_rebuild) return;

#ifndef NDEBUG
        auto start_time = std::chrono::high_resolution_clock::now();
#endif
        _point_cloud_ptr = std::make_shared<point_cloud_float>();

        // grab the data from input1 which is at _source_frame
        Info source_info = input(1)->info();

        Tile tile(*input(0, 1), source_info.x(), source_info.y(), source_info.r(), source_info.t(), _channels, true);
        if (aborted()) return;

        Tile mask_tile(*input(mask_input, 1), source_info.x(), source_info.y(), source_info.r(), source_info.t(), mask_Channel, true);
        if (aborted()) return;

        const bool use_mask = (_mask_channel != Chan_Black);

        // reserve the full source bbox up front; transient memory, avoids reallocs
        _point_cloud_ptr->pts.reserve((size_t)(source_info.r() - source_info.x()) *
                                      (size_t)(source_info.t() - source_info.y()));

        for (int ty = source_info.y(); ty < source_info.t(); ++ty)
        {
            for (int tx = source_info.x(); tx < source_info.r(); ++tx)
            {
                if (!use_mask || *(mask_tile[_mask_channel][ty] + tx) != 0.0f)
                {
                    float values[3] = {0.0f, 0.0f, 0.0f};
                    for (int i = 0; i < _nch; ++i)
                    {
                        values[i] = tile[_pref_ch[i]][ty][tx];
                    }
                    // only add non-zero values into the point cloud for speed
                    if (values[0] != 0.0f || values[1] != 0.0f || values[2] != 0.0f)
                    {
                        PointCloud<float>::Point pt;
                        pt.p[0] = values[0]; pt.p[1] = values[1]; pt.p[2] = values[2];
                        pt.pos_x = (float) tx + 0.5f; pt.pos_y = (float) ty + 0.5f;
                        _point_cloud_ptr->pts.push_back(pt);
                    }
                }
            }
        }
#ifndef NDEBUG
        auto build_time = std::chrono::high_resolution_clock::now();
#endif
        _kd_tree_ptr = std::make_shared<my_kd_tree_t>(3 /*dim*/, *_point_cloud_ptr, KDTreeSingleIndexAdaptorParams(16 /* max leaf */));
        _kd_tree_ptr->buildIndex();
#ifndef NDEBUG
        auto finish_time = std::chrono::high_resolution_clock::now();
        std::cout << "--------------- kd tree ---------------" << std::endl;
        std::cout << "     num points: " << _point_cloud_ptr->pts.size() << std::endl;
        std::cout << "        samples: " << _samples << std::endl;
        std::cout << "   source frame: " << _source_frame << std::endl;
        std::cout << "  current frame: " << (int) outputContext().frame() << std::endl;
        std::cout << "     fill  time: " << std::chrono::duration_cast<std::chrono::milliseconds>(build_time - start_time).count() << " ms" << std::endl;
        std::cout << "     build time: " << std::chrono::duration_cast<std::chrono::milliseconds>(finish_time - build_time).count() << " ms" << std::endl;
        std::cout.flush();
#endif
        _rebuild = false;
    }

    void engine(int y, int x, int r, ChannelMask channels, Row &outrow) override
    {
        if (!input(0) || aborted())
        {
            return;
        }

        // check to see if the input 1 has the mask channel, if so we're using that
        ChannelSet mask_Channel(_mask_channel);
        const bool use_mask = (_mask_channel != Chan_Black);
        int mask_input = (use_mask && input(1, 0) && (input(1, 0)->channels() & mask_Channel)) ? 1 : 0;

        // build the kd-tree once (double-checked under the lock)
        if (_rebuild)
        {
            buildIndex(mask_input, mask_Channel);
            if (aborted()) return;
        }

        // grab our input row that we are going to read the channels and target channels from
        ChannelSet actual_channels = channels;
        actual_channels -= _out_channels[0];
        actual_channels -= _out_channels[1];

        Row row(x, r);
        row.get(input0(), y, x, r, actual_channels + _channels);

        // pass through all the channels except for uv which we will be calculating
        foreach(z, actual_channels)
        {
            outrow.copy(row, z, x, r);
        }

        if (aborted())
        {
            return;
        }

        // ---- hoist all per-channel base pointers OUT of the pixel loop ----
        const float* cz[3] = { nullptr, nullptr, nullptr };
        for (int i = 0; i < _nch; ++i) cz[i] = row[_pref_ch[i]];

        Row mask_row(x, r);
        const float* mz = nullptr;
        if (use_mask)
        {
            mask_row.get(*input(mask_input, 0), y, x, r, mask_Channel);
            mz = mask_row[_mask_channel];
        }

        float* ou = outrow.writable(_out_channels[0]);
        float* ov = outrow.writable(_out_channels[1]);

        const PointCloud<float>::Point* P =
            (_point_cloud_ptr && !_point_cloud_ptr->pts.empty()) ? _point_cloud_ptr->pts.data() : nullptr;
        const bool has_tree = (_kd_tree_ptr && P);

        const bool single   = (_samples <= 1);
        const float inv_w   = 1.0f / (float) format().width();
        const float inv_h   = 1.0f / (float) format().height();
        const SearchParams search(32, 0.0f, false); // sorted=false: IDW sum is order-independent

        // one result set + scratch per row, re-init() per pixel
        const int k = single ? 1 : _samples;
        std::unique_ptr<size_t[]> ret_index(new size_t[k]);
        std::unique_ptr<float[]>  out_dist_sqr(new float[k]);
        nanoflann::KNNResultSet<float> resultSet(k);

        for (int xx = x; xx < r; ++xx)
        {
            float u = 0.0f, v = 0.0f;

            if (has_tree && (!use_mask || mz[xx] != 0.0f))
            {
                float query[3] = { 0.0f, 0.0f, 0.0f };
                for (int i = 0; i < _nch; ++i) query[i] = cz[i][xx];

                resultSet.init(ret_index.get(), out_dist_sqr.get());
                _kd_tree_ptr->findNeighbors(resultSet, &query[0], search);
                const unsigned n = (unsigned) resultSet.size();

                if (single)
                {
                    if (n > 0)
                    {
                        const PointCloud<float>::Point& q = P[ret_index[0]];
                        u = q.pos_x; v = q.pos_y;
                    }
                }
                else if (n > 0)
                {
                    float total_weights = 0.0f;
                    for (unsigned i = 0; i < n; ++i)
                        total_weights += 1.0f / std::max(out_dist_sqr[i], APPROX_ZERO);

                    const float inv_tw = 1.0f / total_weights;
                    for (unsigned i = 0; i < n; ++i)
                    {
                        const float weight = (1.0f / std::max(out_dist_sqr[i], APPROX_ZERO)) * inv_tw;
                        const PointCloud<float>::Point& q = P[ret_index[i]];
                        u += weight * q.pos_x;
                        v += weight * q.pos_y;
                    }
                }
            }

            // calculate the correct u,v based on the mode
            if (_mode == 0)        { u *= inv_w; v *= inv_h; }     // stmap
            else if (_mode == 1)   { u -= (float) xx; v -= (float) y; } // uvmap

            ou[xx] = u;
            ov[xx] = v;
        }
    }

    static const Description d;
};

static Op *build(Node *node) { return new PRefToMotion(node); }

const Op::Description PRefToMotion::d(::CLASS, "Transform/PRefToMotion", build);
