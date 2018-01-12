#include "Halide.h"
#include <string.h>

using namespace Halide;
using std::string;

const unsigned char gaussian2d[5][5] = {
    {1,     3,     6,     3,     1},
    {3,    15,    25,    15,     3},
    {6,    25,    44,    25,     6},
    {3,    15,    25,    15,     3},
    {1,     3,     6,     3,     1}
};

Var x("x"), y("y"), c("c");
Var xo("xo"), xi("xi"), yi("yi"), yo("yo");

class MyPipeline {
    ImageParam input;
    ImageParam weight;
    Param<uint16_t> bias;
    Func kernel_f, kernel;
    Func clamped;
    Func conv1, conv1_shifted;
    Func output;
    Func hw_output;
    std::vector<Argument> args;

    RDom win;

    Func convolve55_rd(Func in) {
        Func local_sum, res;
        RDom r(-2, 5, -2, 5);

        local_sum(x, y, c) = bias;

        local_sum(x, y, c) += cast<uint16_t>(in(x+r.x, y+r.y, c)) * weight(r.x+2, r.y+2);
        res(x, y, c) = cast<uint8_t>(local_sum(x, y, c) >> 8);

        // unroll the reduction
        local_sum.update(0).unroll(r.x).unroll(r.y);
        return res;
    }

public:
    MyPipeline() : input(UInt(8), 3, "input"), weight(UInt(8), 2, "weight"), bias("bias"),
                   kernel("kernel"), conv1("conv1"),
                   output("output"), hw_output("hw_output"),
                   win(-2, 5, -2, 5) {
        float sigma = 1.5f;

        kernel_f(x) = exp(-x*x/(2*sigma*sigma)) / (sqrtf(2*M_PI)*sigma);
        // normalize and convert to 8bit integer
        kernel(x) = cast<uint8_t>(kernel_f(x) * 255 /
                                  (kernel_f(0) + kernel_f(1)*2 + kernel_f(2)*2));
        // precompute kernel values
        kernel.compute_root();

        // define the algorithm
        clamped = BoundaryConditions::repeat_edge(input);
        conv1(x, y, c) += cast<uint32_t>(clamped(x+win.x, y+win.y, c)) * kernel(win.x) * kernel(win.y);
        conv1_shifted(x, y, c) = cast<uint8_t>(conv1(x, y, c) >> 16);

        // unroll the reduction
        conv1.update(0).unroll(c).unroll(win.x).unroll(win.y);

        hw_output = convolve55_rd(conv1_shifted);
        output(x, y, c) = hw_output(x, y, c);

        // constraints
        output.bound(c, 0, 3);

        weight.dim(0).set_bounds(0, 5);
        weight.dim(1).set_bounds(0, 5);
        weight.dim(0).set_stride(1);
        weight.dim(1).set_stride(5);

        args.push_back(input);
        args.push_back(weight);
        args.push_back(bias);

    }

    void compile_cpu() {
        std::cout << "\ncompiling cpu code..." << std::endl;

        output.tile(x, y, xo, yo, xi, yi, 256, 256);
        output.fuse(xo, yo, xo).parallel(xo);

        output.vectorize(xi, 8);
        conv1_shifted.compute_at(output, xo).vectorize(x, 8);

        //output.print_loop_nest();
        output.compile_to_lowered_stmt("pipeline_native.ir.html", args, HTML);
        output.compile_to_header("pipeline_native.h", args, "pipeline_native");
        output.compile_to_object("pipeline_native.o", args, "pipeline_native");

        std::vector<Target::Feature> features({Target::Zynq});
        Target target(Target::Linux, Target::ARM, 32, features);
        output.compile_to_header("pipeline_arm.h", args, "pipeline_native", target);
        output.compile_to_object("pipeline_arm.o", args, "pipeline_native", target);
    }

    void compile_gpu() {
        std::cout << "\ncompiling gpu code..." << std::endl;

        output.compute_root().reorder(x, y, c).gpu_tile(x, y, c, 16, 16, 1);
        conv1_shifted.compute_root().reorder(x, y, c).gpu_tile(x, y, c, 16, 16, 1);
        //conv1.compute_at(output, Var::gpu_blocks()).gpu_threads(x, y, c);
        //output.print_loop_nest();

        Target target = get_target_from_environment();
        target.set_feature(Target::CUDA);
        output.compile_to_lowered_stmt("pipeline_cuda.ir.html", args, HTML, target);
        output.compile_to_header("pipeline_cuda.h", args, "pipeline_cuda", target);
        output.compile_to_object("pipeline_cuda.o", args, "pipeline_cuda", target);
    }

    void compile_hls() {
        std::cout << "\ncompiling HLS code..." << std::endl;

        clamped.compute_root(); // prepare the input for the whole image

        // HLS schedule: make a hw pipeline producing 'hw_output', taking
        // inputs of 'clamped', buffering intermediates at (output, xo) loop
        // level
        hw_output.compute_root();
        hw_output.tile(x, y, xo, yo, xi, yi, 64, 64).reorder(c, xi, yi, xo, yo);
        //hw_output.unroll(xi, 2);
        hw_output.accelerate({clamped}, xi, xo, {kernel});  // define the inputs and the output
        conv1_shifted.linebuffer();
        conv1.unroll(c).unroll(x).unroll(y);
        hw_output.unroll(c);

        //output.print_loop_nest();
        Target hls_target = get_target_from_environment();
        hls_target.set_feature(Target::CPlusPlusMangling);
        output.compile_to_lowered_stmt("pipeline_hls.ir.html", args, HTML, hls_target);
        output.compile_to_hls("pipeline_hls.cpp", args, "pipeline_hls", hls_target);
        output.compile_to_header("pipeline_hls.h", args, "pipeline_hls", hls_target);

        std::vector<Target::Feature> features({Target::Zynq});
        Target target(Target::Linux, Target::ARM, 32, features);
        target.set_feature(Target::CPlusPlusMangling);
        output.compile_to_zynq_c("pipeline_zynq.cpp", args, "pipeline_hls", target);
        output.compile_to_header("pipeline_zynq.h", args, "pipeline_hls", target);
    }
};


int main(int argc, char **argv) {
    MyPipeline p1;
    p1.compile_cpu();

    MyPipeline p2;
    p2.compile_hls();

    MyPipeline p3;
    //p3.compile_gpu();
    return 0;
}
