#include <iostream>
#include <iomanip>
#include <fstream>
#include <limits>
#include <algorithm>

#include "CodeGen_FIRRTL_Target.h"
#include "CodeGen_Internal.h"
#include "Substitute.h"
#include "IRMutator.h"
#include "IROperator.h"
#include "Param.h"
#include "Var.h"
#include "Lerp.h"
#include "Simplify.h"

namespace Halide {
namespace Internal {

using std::ostream;
using std::endl;
using std::string;
using std::vector;
using std::ostringstream;
using std::ofstream;

namespace {

class ContainForLoop : public IRVisitor {
    using IRVisitor::visit;
    void visit(const For *op) {
        found = true;
        return;
    }

public:
    bool found;

    ContainForLoop() : found(false) {}
};

bool contain_for_loop(Stmt s) {
    ContainForLoop cfl;
    s.accept(&cfl);
    return cfl.found;
}

class ContainReadStream : public IRVisitor {
    using IRVisitor::visit;
    void visit(const Call *op) {
        if(op->name == "read_stream") {
            found = true;
        }
        return;
    }

public:
    bool found;

    ContainReadStream() : found(false) {}
};

bool contain_read_stream(Stmt s) {
    ContainReadStream cfl;
    s.accept(&cfl);
    return cfl.found;
}

class ContainWriteStream : public IRVisitor {
    using IRVisitor::visit;
    void visit(const Call *op) {
        if(op->name == "write_stream") {
            found = true;
        }
        return;
    }

public:
    bool found;

    ContainWriteStream() : found(false) {}
};

bool contain_write_stream(Stmt s) {
    ContainWriteStream cfl;
    s.accept(&cfl);
    return cfl.found;
}

class ContainRealize : public IRVisitor {
    using IRVisitor::visit;
    void visit(const Realize *op) {
        found = true;
        return;
    }

public:
    bool found;

    ContainRealize() : found(false) {}
};

bool contain_realize(Stmt s) {
    ContainRealize cfl;
    s.accept(&cfl);
    return cfl.found;
}

}

// Extract Params and tap.stencils used in the For loop to make port of them.
class FIRRTL_For_Closure : public Closure {
public:
    FIRRTL_For_Closure(Stmt s)  {
        s.accept(this);
    }

    vector<string> arguments(void);

protected:
    using Closure::visit;
    void visit(const Call *);

};

void FIRRTL_For_Closure::visit(const Call *op)
{
    // Ignore read_stream and write_stream because they're taken care of 
    // by CodeGen_FIRRTL_Target::visit(Call).
    if((op->name != "read_stream") &&
       (op->name != "write_stream")) {
        Closure::visit(op);
    }
}

vector<string> FIRRTL_For_Closure::arguments(void) {
    vector<string> res;
    for (const pair<string, Type> &i : vars) {
        if(ends_with(i.first, ".stream") ) {
            internal_error << "we don't expect to see a stream type in FIRRTL_For_Closure.\n";
        } else if(ends_with(i.first, ".stencil") ) {
            res.push_back({i.first});
        } else if (ends_with(i.first, ".stencil_update")) {
            internal_error << "we don't expect to see a stencil_update type in FIRRTL_For_Closure.\n";
        } else {
            // it is a scalar variable
            res.push_back({i.first});
        }
    }
    return res;
}

CodeGen_FIRRTL_Target::CodeGen_FIRRTL_Target(std::ostream &s, Target t, const std::string &ip_name)
    : IRPrinter(s), id("$$ BAD ID $$"), target(t), target_name(ip_name) {
    indent = 0;
    // initialize the source file
    stream << ";Generated FIRRTL\n";
    stream << ";Target name: " << target_name << "\n";
}

// Extract root of the name
string CodeGen_FIRRTL_Target::rootName(const string &name)
{
    ostringstream oss;

    for (size_t i = 0; i < name.size(); i++) {
        if (name[i]=='.') break;
        oss << name[i];
    }
    return oss.str();
}

string CodeGen_FIRRTL_Target::print_name(const string &name) {
    ostringstream oss;

    for (size_t i = 0; i < name.size(); i++) {
        if (!isalnum(name[i])) {
            oss << "_";
        }
        else oss << name[i];
    }
    return oss.str();
}

string CodeGen_FIRRTL_Target::print_expr(Expr e) {
    id = "$$ BAD ID $$";
    e.accept(this);
    return id;
}

void CodeGen_FIRRTL_Target::print_stmt(Stmt s) {
    s.accept(this);
}

string CodeGen_FIRRTL_Target::print_base_type(const Type type) {
    ostringstream oss;

    if (type.is_uint()) oss << "U";
    else oss << "S";

    oss << "Int";

    return oss.str();
}

string CodeGen_FIRRTL_Target::print_type(const Type type) {
    ostringstream oss;

    if (type.is_uint()) oss << "U";
    else oss << "S";

    oss << "Int<" << type.bits() << ">";

    return oss.str();
}

string CodeGen_FIRRTL_Target::print_stencil_type(FIRRTL_Type stencil_type) {
    ostringstream oss;
    // C: Stencil<uint16_t, 1, 1, 1> stencil_var;
    // FIRRTL: UInt<16>[1][1][1]
    // C: hls::stream<Stencil<uint16_t, 1, 1, 1> > stencil_stream_var;
    // FIRRTL: {value: UInt<16>[1][1][1], valid: UInt<1>, flip ready: UInt<1>}
    // C: hls::stream<AxiPackedStencil<uint16_t, 1, 1, 1> > stencil_stream_var;
    // FIRRTL: {TDATA: UInt<16>[1][1][1], TVALID: UInt<1>, TLAST: UInt<1>, flip TREADY: UInt<1>}

    switch(stencil_type.type) {
    case FIRRTL_Type::StencilContainerType::Scalar :
        oss << print_type(stencil_type.elemType);
        break;
    case FIRRTL_Type::StencilContainerType::Stencil :
        oss << print_type(stencil_type.elemType);
        for(const auto &range : stencil_type.bounds) {
            internal_assert(is_one(simplify(range.min == 0)));
            oss << "[" << range.extent << "]";
        }
        break;
    case FIRRTL_Type::StencilContainerType::Stream :
        oss << "{value : ";
        oss << print_type(stencil_type.elemType);
        for(const auto &range : stencil_type.bounds) {
            internal_assert(is_one(simplify(range.min == 0)));
            oss << "[" << range.extent << "]";
        }
        oss << ", valid : UInt<1>, ";
        oss << "flip ready : UInt<1>}" ;

        break;
    case FIRRTL_Type::StencilContainerType::AxiStream :
        oss << "{TDATA : ";
        oss << print_type(stencil_type.elemType);
        for(const auto &range : stencil_type.bounds) {
            internal_assert(is_one(simplify(range.min == 0)));
            oss << "[" << range.extent << "]";
        }
        oss << ", TVALID : UInt<1>, ";
        oss << "flip TREADY : UInt<1>, " ;
        oss << "TLAST : UInt<1>}"; // AXI-S

        break;
    case FIRRTL_Type::StencilContainerType::MemRd :
        oss << "{value : ";
        oss << print_type(stencil_type.elemType);
        oss << ", flip addr : UInt<32>";
        //oss << "[" << stencil_type.bounds.size() << "]}";
        oss << "[4]}"; // support up to 4D tap stencil

        break;
    default: internal_error;
    }
    return oss.str();
}

string CodeGen_FIRRTL_Target::print_assignment(Type t, const std::string &rhs) {

    map<string, string>::iterator cached = cache.find(rhs);

    id = unique_name('_');

    if (cached == cache.end()) {
        if (current_fb!=nullptr) { // Inside ForBlock, print to ForBlock oss_body directly.
            current_fb->print("node " + id + " = " + rhs + "\n");
        } else {
            FIRRTL_Type wire_type = {FIRRTL_Type::StencilContainerType::Scalar,t,Region(),0,{}};
            top->addWire(id, wire_type);
            top->addConnect(id, rhs);
        }
        cache[rhs] = id;
    } else {
        id = cached->second;
    }
    return id;
}

void CodeGen_FIRRTL_Target::add_kernel(Stmt stmt,
                                       const vector<FIRRTL_Argument> &args) {
    // Create Top module
    top = new TopLevel("hls_target");
    sif = new SlaveIf("SlaveIf");
    top->addInstance(static_cast<Component*>(sif));
    top->addConnect(sif->getInstanceName() + ".clock", "clock");
    top->addConnect(sif->getInstanceName() + ".reset", "reset");

    // Create some global wires
    FIRRTL_Type wire_1bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(1),Region(),0,{}};
    FIRRTL_Type wire_2bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(2),Region(),0,{}};
    FIRRTL_Type wire_4bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(4),Region(),0,{}};
    FIRRTL_Type wire_32bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(32),Region(),0,{}};
    sif->addOutPort("start", wire_1bit);

    // Create Slave Bus Interface (AXIS)
    top->addInPort("AWADDR", wire_32bit); // TODO: optimal width?
    sif->addInPort("AWADDR", wire_32bit); // TODO: optimal width?
    top->addConnect(sif->getInstanceName()+ ".AWADDR", "AWADDR");
    top->addInPort("AWVALID", wire_1bit);
    sif->addInPort("AWVALID", wire_1bit);
    top->addConnect(sif->getInstanceName()+ ".AWVALID", "AWVALID");
    top->addOutPort("AWREADY", wire_1bit);
    sif->addOutPort("AWREADY", wire_1bit);
    top->addConnect("AWREADY", sif->getInstanceName()+ ".AWREADY");
    top->addInPort("WVALID", wire_1bit);
    sif->addInPort("WVALID", wire_1bit);
    top->addConnect(sif->getInstanceName()+ ".WVALID", "WVALID");
    top->addOutPort("WREADY", wire_1bit);
    sif->addOutPort("WREADY", wire_1bit);
    top->addConnect("WREADY", sif->getInstanceName()+ ".WREADY");
    top->addInPort("WDATA", wire_32bit); // supports 32-bit data bus only
    sif->addInPort("WDATA", wire_32bit); // supports 32-bit data bus only
    top->addConnect(sif->getInstanceName()+ ".WDATA", "WDATA");
    top->addInPort("WSTRB", wire_4bit);
    sif->addInPort("WSTRB", wire_4bit);
    top->addConnect(sif->getInstanceName()+ ".WSTRB", "WSTRB");

    top->addInPort("ARADDR", wire_32bit); // TODO: optimal width?
    sif->addInPort("ARADDR", wire_32bit); // TODO: optimal width?
    top->addConnect(sif->getInstanceName()+ ".ARADDR", "ARADDR");
    top->addInPort("ARVALID", wire_1bit);
    sif->addInPort("ARVALID", wire_1bit);
    top->addConnect(sif->getInstanceName()+ ".ARVALID", "ARVALID");
    top->addOutPort("ARREADY", wire_1bit);
    sif->addOutPort("ARREADY", wire_1bit);
    top->addConnect("ARREADY", sif->getInstanceName()+ ".ARREADY");
    top->addOutPort("RVALID", wire_1bit);
    sif->addOutPort("RVALID", wire_1bit);
    top->addConnect("RVALID", sif->getInstanceName()+ ".RVALID");
    top->addInPort("RREADY", wire_1bit);
    sif->addInPort("RREADY", wire_1bit);
    top->addConnect(sif->getInstanceName()+ ".RREADY", "RREADY");
    top->addOutPort("RDATA", wire_32bit); // supports 32-bit data bus only
    sif->addOutPort("RDATA", wire_32bit); // supports 32-bit data bus only
    top->addConnect("RDATA", sif->getInstanceName()+ ".RDATA");
    top->addOutPort("RRESP", wire_2bit);
    sif->addOutPort("RRESP", wire_2bit);
    top->addConnect("RRESP", sif->getInstanceName()+ ".RRESP");

    top->addOutPort("BVALID", wire_1bit);
    sif->addOutPort("BVALID", wire_1bit);
    top->addConnect("BVALID", sif->getInstanceName()+ ".BVALID");
    top->addInPort("BREADY", wire_1bit);
    sif->addInPort("BREADY", wire_1bit);
    top->addConnect(sif->getInstanceName()+ ".BREADY", "BREADY");
    top->addOutPort("BRESP", wire_2bit);
    sif->addOutPort("BRESP", wire_2bit);
    top->addConnect("BRESP", sif->getInstanceName()+ ".BRESP");

    // Process for each input/output.
    for (size_t i = 0; i < args.size(); i++) {
        FIRRTL_Type stype = args[i].stencil_type;
        debug(3) << "add_kernel: " << args[i].name << " " << print_stencil_type(stype) << "\n";
        bool is_stream = (args[i].stencil_type.type == FIRRTL_Type::StencilContainerType::Stream) ||
                         (args[i].stencil_type.type == FIRRTL_Type::StencilContainerType::AxiStream);
        bool is_stencil = args[i].stencil_type.type == FIRRTL_Type::StencilContainerType::Stencil;
        if (is_stream) { // is stream (all streams are stream of stencils).
            internal_assert(stype.type == FIRRTL_Type::StencilContainerType::AxiStream); // The very input to DUT is expected to be AxiStream for now.

            string stream_name = print_name(args[i].name);
            debug(3) << "add_kernel: stream_name " << stream_name << "\n";

            FIRRTL_Type stream_type = stype;
            stream_type.type = FIRRTL_Type::StencilContainerType::Stream; // protocol change from AXIS(TDATA,TVALID,TREADY,TLAST) to Stream(value,valid,ready)
            debug(3) << "stream_type: " << print_stencil_type(stream_type) << "\n";
            if (!args[i].is_output) { // Input IO
                // Create IO component for each input and output
                IO *interface = new IO("IO_" + stream_name, ComponentType::Input);

                // Add to top
                top->addInstance(static_cast<Component*>(interface));

                string arg_name = print_name(rootName(args[i].name)); // Use simple name for input.
                interface->addInput(arg_name, stype); // axi stream
                interface->addOutput(stream_name, stream_type);
                interface->setStoreExtents(stream_type.store_extents);
                top->addInput(arg_name, stype);
                //numInputs++;

                // Connect clock/reset
                top->addConnect(interface->getInstanceName() + ".clock", "clock");
                top->addConnect(interface->getInstanceName() + ".reset", "reset");

                // Connect IO input port
                top->addConnect(interface->getInstanceName() + "." + arg_name, arg_name);                          // IO.data_in <= in

                // Connect IO Start/Done
                string done = "IO_" + stream_name + "_done";
                sif->addInPort(done, wire_1bit);
                interface->addInPort("start_in", wire_1bit);
                interface->addOutPort("done_out", wire_1bit);
                top->addConnect(interface->getInstanceName() + ".start_in", sif->getInstanceName() + ".start");    // IO.start_in <= SIF.start
                top->addConnect(sif->getInstanceName() + "." + done, interface->getInstanceName() + ".done_out");  // SIF.done <= IO.done_out

                // Create FIFO following IO
                FIFO *fifo = new FIFO("FIFO_" + stream_name);
                fifo->addInput("data_in", stream_type); // Use data_in, data_out for port name for re-useability.
                fifo->addOutput("data_out", stream_type);

                // Add to top
                top->addInstance(static_cast<Component*>(fifo));

                // Connect clock/reset
                top->addConnect(fifo->getInstanceName() + ".clock", "clock");
                top->addConnect(fifo->getInstanceName() + ".reset", "reset");

                // Connect FIFO input port
                top->addConnect(fifo->getInstanceName() + ".data_in", interface->getInstanceName() + "." + stream_name);

                // Connect FIFO output port
                top->addWire("wire_" + stream_name, stream_type); // Keep FIFO output as a wire.
                top->addConnect("wire_" + stream_name, fifo->getInstanceName() + ".data_out"); // wire <= FIFO.data_out
            } else { // Output IO
                top->addWire("wire_" + print_name(args[i].name), stream_type);
                // Adding output IO when "write_stream" with more than 2 args are processed.
            }

        } else if (is_stencil) { // stencil, mapped to memory inside SlaveIf.
            string s = print_name(args[i].name);
            sif->addReg("r_" + s, stype);
            // Wire will be added and connected for each reference.
            //sif->addOutPort(s, stype);
            stype.type = FIRRTL_Type::StencilContainerType::MemRd;
            top->addWire("wire_" + s, stype); // TODO use Scope<>
            //top->addConnect("wire_" + s, sif->getInstanceName() + "." + s);
        } else { // constant scalar or stencil
            string s = print_name(args[i].name);
            sif->addOutPort(s, stype);
            sif->addReg("r_" + s, stype);
            top->addWire("wire_" + s, stype);
            top->addConnect("wire_" + s, sif->getInstanceName() + "." + s);
        }
    }

    // initialize
    current_fb = nullptr;

    // Visit body to collect components.
    print(stmt);

    // Print collected component in FIRRTL.

    stream << "circuit " + top->getInstanceName() + " :\n";
    open_scope();

    do_indent();
    stream << "; Top instance " << top->getInstanceName() << "\n";
    print_module(top);
    stream << "; SlaveIf instance " << sif->getInstanceName() << "\n";
    print_slaveif(sif);

    for(auto &c : top->getComponents(ComponentType::Input)) {
        do_indent();
        stream << "; Input instance " << c->getInstanceName() << "\n";
        print_io(static_cast<IO*>(c));
    }
    for(auto &c : top->getComponents(ComponentType::Output)) {
        do_indent();
        stream << "; Output instance " << c->getInstanceName() << "\n";
        print_io(static_cast<IO*>(c));
    }
    for(auto &c : top->getComponents(ComponentType::Fifo)) {
        do_indent();
        stream << "; FIFO instance " << c->getInstanceName() << "\n";
        print_fifo(static_cast<FIFO*>(c));
    }
    for(auto &c : top->getComponents(ComponentType::Linebuffer)) {
        do_indent();
        stream << "; Linebuffer instance " << c->getInstanceName() << "\n";
        print_linebuffer(static_cast<LineBuffer*>(c));
    }
    for(auto &c : top->getComponents(ComponentType::Dispatcher)) {
        do_indent();
        stream << "; Dispatch instance " << c->getInstanceName() << "\n";
        print_dispatch(static_cast<Dispatch*>(c));
    }
    for(auto &c : top->getComponents(ComponentType::Forblock)) {
        do_indent();
        stream << "; ForBlock instance " << c->getInstanceName() << "\n";
        print_forblock(static_cast<ForBlock*>(c));
    }

}

void CodeGen_FIRRTL_Target::print_module(Component *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";
    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    // Print instances.
    do_indent();
    stream << "; Instances\n";
    for(auto &p : c->getInstances()) {
        do_indent();
        stream << "inst " << p.first << " of " << p.second << "\n";
    }
    stream << "\n";

    // Print Regs.
    do_indent();
    stream << "; Regs\n";
    for(auto &p : c->getRegs()) {
        do_indent();
        stream << "reg " << p.first << " : " << print_stencil_type(p.second) << ", clock\n";
    }
    stream << "\n";

    // Print Wires.
    do_indent();
    stream << "; Wires\n";
    for(auto &p : c->getWires()) {
        do_indent();
        stream << "wire " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    for(auto &p : c->getWires()) {
        do_indent();
        stream << p.first << " is invalid\n";
    }
    stream << "\n";

    // Print connections.
    do_indent();
    stream << "; Connections\n";
    map<string, string> cn = c->getConnects();
    for(auto &p : c->getConnectKeys()) {
        do_indent();
        stream << p << " <= " << cn[p] << "\n";
    }
    close_scope(" end of " + c->getModuleName());
    stream << "\n";
}

void CodeGen_FIRRTL_Target::print_slaveif(SlaveIf *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";

    FIRRTL_Type in_stencil;
    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    FIRRTL_Type out_stencil;
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    // Offset address assignment
    int offset = 0x40; // Base of config registers
    std::map<int, string> complete_address_map; // Just for Register Map table printing.
    std::map<string, Reg_Type> address_map; // map of vector (name, size)
    for(auto &p : c->getRegs()) {
        FIRRTL_Type s = p.second;
        Reg_Type r;
        if (s.type == FIRRTL_Type::StencilContainerType::Stencil) { // TODO: pack it in 32-bit word
            internal_assert(s.bounds.size() <= 4);
            internal_assert(s.bounds.size() >= 1);
            r.extents.push_back(1);
            r.extents.push_back(1);
            r.extents.push_back(1);
            r.extents.push_back(1);
            int bsize = s.bounds.size();
            r.range = 1;
            for(int i = 0 ; i < bsize ; i++) {
                const IntImm *e = s.bounds[i].extent.as<IntImm>();
                r.extents[i] = e->value;
                r.range *= e->value;
            }
            r.is_stencil = true;
            r.bitwidth = 32; //s.elemType.bits(); // TODO for packing
            //TODO for packing... r.range *= r.bitwidth;
            r.range *= 4; // range in byte
            r.offset = offset;
            address_map[p.first] = r;
            string regidx0, regidx1, regidx2, regidx3;
            for(int i3 = 0; i3 < r.extents[3]; i3++) {
                regidx3 = "_" + std::to_string(i3);
                for(int i2 = 0; i2 < r.extents[2]; i2++) {
                    regidx2 = "_" + std::to_string(i2);
                    for(int i1 = 0; i1 < r.extents[1]; i1++) {
                        regidx1 = "_" + std::to_string(i1);
                        for(int i0 = 0; i0 < r.extents[0]; i0++) {
                            regidx0 = "_" + std::to_string(i0);
                            string n = p.first;
                            n.replace(0,2,""); // remove "r_"
                            complete_address_map[offset] = n+regidx3+regidx2+regidx1+regidx0; // reverse-order
                            offset += 4; // TODO: packing
                        }
                    }
                }
            }
        } else {
            r.is_stencil = false;
            r.bitwidth = 32;//s.elemType.bits(); // TODO for packing
            r.range = 4; // range in byte
            r.offset = offset;
            string n = p.first;
            n.replace(0,2,""); // remove "r_"
            complete_address_map[offset] = n;
            address_map[p.first] = r;
            offset += 4;
        }
    }

    // Body
    do_indent(); stream << ";------------------ Start of Register Map -----------------\n";
    do_indent(); stream << "; 0x00000000 : CTRL\n";
    do_indent(); stream << ";              [0]: Start (Write 1 to start, auto cleared)\n";
    do_indent(); stream << ";              [1]: Done (Set to 1 when all block are done. Write 1 to clear)\n";
    do_indent(); stream << "; 0x00000004 : STATUS (Read-Only)\n";
    do_indent(); stream << ";              [0]: Run (1 indicates running).\n";
    do_indent(); stream << "; 0x00000008 : Interrupt Enable // TODO\n";
    do_indent(); stream << "; 0x0000000C : Interrupt Status // TODO\n";
    do_indent(); stream << "; 0x00000010 : Info0 (Read-Only) // TODO: such as loop count?\n";
    do_indent(); stream << "; 0x00000014 : Info1 (Read-Only)\n";
    do_indent(); stream << "; 0x000000l8 : Info2 (Read-Only)\n";
    do_indent(); stream << "; 0x000000lC : Info3 (Read-Only)\n";
    do_indent(); stream << "; 0x00000020 : Info4 (Read-Only)\n";
    do_indent(); stream << "; 0x00000024 : Info5 (Read-Only)\n";
    do_indent(); stream << "; 0x00000028 : Info6 (Read-Only)\n";
    do_indent(); stream << "; 0x0000002C : Info7 (Read-Only)\n";
    for(auto &p : complete_address_map) { // sort by address
        do_indent();
        stream << "; 0x" << std::hex << std::setw(8) << std::setfill('0') << p.first << " : " << p.second << "\n";
    }
    do_indent(); stream << ";------------------ End of Register Map -----------------\n";
    stream << std::dec;
    stream << "\n";

    do_indent(); stream << "wire ST_AW_IDLE : UInt<2>\n";
    do_indent(); stream << "wire ST_AW_ADDR : UInt<2>\n";
    do_indent(); stream << "wire ST_AW_DATA : UInt<2>\n";
    do_indent(); stream << "wire ST_AR_IDLE : UInt<2>\n";
    do_indent(); stream << "wire ST_AR_ADDR : UInt<2>\n";
    do_indent(); stream << "wire ST_AR_DATA : UInt<2>\n";
    stream << "\n";
    do_indent(); stream << "ST_AW_IDLE <= UInt<2>(0)\n";
    do_indent(); stream << "ST_AW_ADDR <= UInt<2>(1)\n";
    do_indent(); stream << "ST_AW_DATA <= UInt<2>(2)\n";
    do_indent(); stream << "ST_AR_IDLE <= UInt<2>(0)\n";
    do_indent(); stream << "ST_AR_ADDR <= UInt<2>(1)\n";
    do_indent(); stream << "ST_AR_DATA <= UInt<2>(2)\n";
    stream << "\n";
    do_indent(); stream << "wire ADDR_CTRL : UInt<32>\n";
    do_indent(); stream << "wire ADDR_STATUS : UInt<32>\n";
    stream << "\n";
    do_indent(); stream << "ADDR_CTRL <= UInt<32>(0)\n";
    do_indent(); stream << "ADDR_STATUS <= UInt<32>(4)\n";
    stream << "\n";

    do_indent(); stream << "reg  r_aw_cs_fsm : UInt<2>, clock with : (reset => (reset, UInt<2>(0)))\n";
    do_indent(); stream << "wire w_aw_ns_fsm : UInt<2>\n";
    do_indent(); stream << "reg  r_aw_addr : UInt<32>, clock\n";
    do_indent(); stream << "reg  r_ar_cs_fsm : UInt<2>, clock with : (reset => (reset, UInt<2>(0)))\n";
    do_indent(); stream << "wire w_ar_ns_fsm : UInt<2>\n";
    do_indent(); stream << "reg  r_ar_addr : UInt<32>, clock\n";
    do_indent(); stream << "reg  r_rd_data : UInt<32>, clock\n";
    do_indent(); stream << "reg  r_start : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg  r_run :   UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg  r_done :  UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    vector<string> done_ports;
    for(auto &p : c->getInPorts()) {
        if (ends_with(p.first, "_done")) {// collecting done signals
            done_ports.push_back(p.first);
            do_indent();
            stream << "reg  r_" << p.first << " : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
        }
    }
    for(auto &p : c->getRegs()) {
        FIRRTL_Type s = p.second;
        Reg_Type r = address_map[p.first];
        if (s.type != FIRRTL_Type::StencilContainerType::Stencil) {
            do_indent();
            stream << "reg  " << p.first << " : " << print_stencil_type(p.second) << ", clock";
            stream << " with : (reset => (reset, " << print_type(s.elemType) << "(0)))\n";
        } else {
            do_indent();
            stream << "cmem " << p.first << " : {value : " << print_type(s.elemType) << "}[" << (r.range>>2) << "]\n"; // >>2 to word count.
            do_indent(); stream << "wire w_" << p.first << "_rd_idx : UInt<32>\n";
            do_indent(); stream << "wire w_" << p.first << "_wr_idx : UInt<32>\n";
        }
    }
    stream << "\n";

    do_indent(); stream << "r_aw_cs_fsm <= w_aw_ns_fsm\n";
    do_indent(); stream << "w_aw_ns_fsm <= r_aw_cs_fsm\n";
    do_indent(); stream << "when eq(r_aw_cs_fsm, ST_AW_IDLE) :\n";
    do_indent(); stream << "  when AWVALID :\n";
    do_indent(); stream << "    w_aw_ns_fsm <= ST_AW_ADDR\n";
    do_indent(); stream << "else when eq(r_aw_cs_fsm, ST_AW_ADDR) :\n";
    do_indent(); stream << "  when WVALID :\n";
    do_indent(); stream << "    w_aw_ns_fsm <= ST_AW_DATA\n";
    do_indent(); stream << "else when eq(r_aw_cs_fsm, ST_AW_DATA) :\n";
    do_indent(); stream << "  when BREADY :\n";
    do_indent(); stream << "    w_aw_ns_fsm <= ST_AW_IDLE\n";
    stream << "\n";

    do_indent(); stream << "when and(AWVALID, AWREADY) :\n";
    do_indent(); stream << "  r_aw_addr <= AWADDR\n";
    stream << "\n";
    do_indent(); stream << "AWREADY <= eq(r_aw_cs_fsm, ST_AW_IDLE)\n";
    do_indent(); stream << "WREADY <= eq(r_aw_cs_fsm, ST_AW_ADDR)\n";
    do_indent(); stream << "BVALID <= eq(r_aw_cs_fsm, ST_AW_DATA)\n";
    do_indent(); stream << "BRESP  <= UInt<1>(0)\n";
    stream << "\n";
    do_indent(); stream << "r_ar_cs_fsm <= w_ar_ns_fsm\n";
    stream << "\n";
    do_indent(); stream << "w_ar_ns_fsm <= r_ar_cs_fsm\n";
    do_indent(); stream << "when eq(r_ar_cs_fsm, ST_AR_IDLE) :\n";
    do_indent(); stream << "  when ARVALID :\n";
    do_indent(); stream << "    w_ar_ns_fsm <= ST_AR_ADDR\n";
    do_indent(); stream << "else when eq(r_ar_cs_fsm, ST_AR_ADDR) :\n";
    do_indent(); stream << "  when RREADY :\n";
    do_indent(); stream << "    w_ar_ns_fsm <= ST_AR_DATA\n";
    do_indent(); stream << "else when eq(r_ar_cs_fsm, ST_AR_DATA) :\n";
    do_indent(); stream << "  w_ar_ns_fsm <= ST_AR_IDLE\n";
    stream << "\n";
    do_indent(); stream << "when and(ARVALID, ARREADY) :\n";
    do_indent(); stream << "  r_ar_addr <= ARADDR\n";
    stream << "\n";
    for(auto &p : address_map) {
        Reg_Type r = p.second;
        if (r.is_stencil) {
            do_indent();
            stream << "w_" << p.first << "_rd_idx" << " <= shr(asUInt(sub(r_ar_addr, UInt(\"h" << std::hex << r.offset << "\"))), 2)\n";
            do_indent();
            stream << "w_" << p.first << "_wr_idx" << " <= shr(asUInt(sub(r_aw_addr, UInt(\"h" << std::hex << r.offset << "\"))), 2)\n";
            stream << std::dec;
        }
    }
    stream << "\n";
    do_indent(); stream << "ARREADY <= eq(r_ar_cs_fsm, ST_AR_IDLE)\n";
    do_indent(); stream << "RRESP <= UInt<2>(0)\n";
    do_indent(); stream << "RVALID <= eq(r_ar_cs_fsm, ST_AR_DATA)\n";
    stream << "\n";
    do_indent(); stream << "when eq(r_ar_cs_fsm, ST_AR_ADDR) :\n";
    do_indent(); stream << "  when eq(r_ar_addr, ADDR_CTRL) :\n";
    do_indent(); stream << "    r_rd_data <= or(shl(r_done, 1), r_start)\n";
    do_indent(); stream << "  else when eq(r_ar_addr, ADDR_STATUS) :\n";
    do_indent(); stream << "    r_rd_data <= r_run\n";
    for(auto &p : address_map) {
        Reg_Type r = p.second;
        if (r.is_stencil) {
            do_indent();
            stream << "  else when and(geq(r_ar_addr, UInt<32>(\"h" << std::hex << r.offset << "\")), ";
            stream << "lt(r_ar_addr, UInt<32>(\"h" << (r.offset + r.range) << "\"))) :\n";
            stream << std::dec;
            do_indent();
            stream << "    infer mport " << p.first << "_rd = " << p.first << "[w_" << p.first << "_rd_idx], clock\n";
            do_indent();
            stream << "    r_rd_data <= asUInt(" << p.first << "_rd.value)\n";
        } else {
            do_indent();
            stream << "  else when eq(r_ar_addr, UInt<32>(\"h" << std::hex << r.offset << "\")) :\n";
            do_indent();
            stream << "    r_rd_data <= asUInt(" << p.first << ")\n";
            stream << std::dec;
        }
    }
    stream << "\n";

    do_indent(); stream << "RDATA <= r_rd_data\n";

    do_indent(); stream << "when and(eq(r_aw_cs_fsm, ST_AW_ADDR), eq(r_aw_addr, ADDR_CTRL)) :\n";
    do_indent(); stream << "  r_start <= WDATA ; bit 0 only\n";
    do_indent(); stream << "else :\n";
    do_indent(); stream << "  r_start <= UInt<1>(0)\n";
    stream << "\n";

    do_indent(); stream << "start <= r_start\n";
    stream << "\n";

    do_indent(); stream << "when r_start :\n";
    do_indent(); stream << "  r_run <= UInt<1>(1)\n";
    do_indent(); stream << "else when r_done :\n";
    do_indent(); stream << "  r_run <= UInt<1>(0)\n";
    stream << "\n";

    for(auto &p : done_ports) {
        do_indent(); stream << "when and(eq(r_aw_cs_fsm, ST_AW_ADDR), and(eq(r_aw_addr, ADDR_CTRL), eq(and(WDATA,UInt<32>(2)), UInt<32>(2)))) :\n";
        do_indent(); stream << "    r_" << p << " <= UInt<1>(0)\n";
        do_indent(); stream << "else when " << p << " :\n";
        do_indent(); stream << "    r_" << p << " <= UInt<1>(1)\n";
        stream << "\n";
    }

    do_indent(); stream << "when and(eq(r_aw_cs_fsm, ST_AW_ADDR), and(eq(r_aw_addr, ADDR_CTRL), eq(and(WDATA,UInt<32>(2)), UInt<32>(2)))) :\n";
    do_indent(); stream << "  r_done <= UInt<1>(0)\n";
    do_indent(); stream << "else when";
    int done_ports_size = done_ports.size();
    for(int i = 0; i < done_ports_size; i++) {
        if (i == (done_ports_size-1)) stream << "r_" << done_ports[i];
        else stream << " and(r_" << done_ports[i] << ", ";
    }
    for(int i = 0; i < done_ports_size-1; i++) {
        stream << ")"; //closing and
    }
    stream << " :\n";
    do_indent(); stream << "  r_done <= UInt<1>(1)\n";
    stream << "\n";

    for(auto &p : address_map) {
        Reg_Type r = p.second;
        FIRRTL_Type s = c->getReg(p.first);
        if (r.is_stencil) {
            do_indent(); stream << "when eq(r_aw_cs_fsm, ST_AW_ADDR) :\n";
            do_indent(); stream << "  when and(geq(r_aw_addr, UInt<32>(\"h" << std::hex << r.offset << "\")), ";
            stream << "lt(r_aw_addr, UInt<32>(\"h" << std::hex << (r.offset + r.range) << "\"))) :\n";
            do_indent(); stream << "    infer mport " << p.first << "_wr = " << p.first << "[w_" << p.first << "_wr_idx], clock\n";
            do_indent(); stream << "    " << p.first << "_wr.value <= as" << print_base_type(s.elemType) << "(WDATA)\n";
        } else {
            do_indent(); stream << "when eq(r_aw_cs_fsm, ST_AW_ADDR) :\n";
            do_indent(); stream << "  when eq(r_aw_addr, UInt<32>(\"h" << std::hex << r.offset << "\")) :\n";
            do_indent(); stream << "    " << p.first << " <= as" << print_base_type(s.elemType) << "(WDATA)\n";
        }
        stream << "\n";
    }
    stream << std::dec;
    stream << "\n";

    for(auto &p : c->getRegs()) {
        string s = p.first;
        Reg_Type r = address_map[p.first];
        s.replace(0,2,""); // remove "r_"
        if (r.is_stencil) { // 
            for(auto &o : c->getOutPorts()) { // search for all output ports related to this config register which is mapped to cmem.
                if (starts_with(o.first, s)) { // TODO: use exact bitwidth.
                    int dim = 1; // do some simplification depending on the dimension.
                    if (r.extents[3]!=1) dim = 4;
                    else if (r.extents[2]!=1) dim = 3;
                    else if (r.extents[1]!=1) dim = 2;
                    do_indent();
                    stream << "node " << o.first << "_idx0 = "                                                                        << o.first << ".addr[0]\n";
                    if (dim==2) {
                        do_indent();
                        stream << "node " << o.first << "_idx1 = mul(UInt<32>(" << std::to_string(r.extents[0]                          ) << "), " << o.first << ".addr[1])\n";
                        do_indent();
                        stream << "node " << o.first << "_idx = add(" << o.first << "_idx1, " << o.first << "_idx0)\n";
                    } else if (dim==3) {
                        do_indent();
                        stream << "node " << o.first << "_idx1 = mul(UInt<32>(" << std::to_string(r.extents[0]                          ) << "), " << o.first << ".addr[1])\n";
                        do_indent();
                        stream << "node " << o.first << "_idx2 = mul(UInt<32>(" << std::to_string(r.extents[0]*r.extents[1]             ) << "), " << o.first << ".addr[2])\n";
                        do_indent();
                        stream << "node " << o.first << "_idx = add(add(" << o.first << "_idx2, " << o.first << "_idx1), " << o.first << "_idx0)\n";
                    } else if (dim==4) {
                        do_indent();
                        stream << "node " << o.first << "_idx1 = mul(UInt<32>(" << std::to_string(r.extents[0]                          ) << "), " << o.first << ".addr[1])\n";
                        do_indent();
                        stream << "node " << o.first << "_idx2 = mul(UInt<32>(" << std::to_string(r.extents[0]*r.extents[1]             ) << "), " << o.first << ".addr[2])\n";
                        do_indent();
                        stream << "node " << o.first << "_idx3 = mul(UInt<32>(" << std::to_string(r.extents[0]*r.extents[1]*r.extents[2]) << "), " << o.first << ".addr[3])\n";
                        do_indent();
                        stream << "node " << o.first << "_idx = add(add(add(" << o.first << "_idx3, " << o.first << "_idx2), " << o.first << "_idx1), " << o.first << "_idx0)\n";
                    } else {
                        do_indent();
                        stream << "node " << o.first << "_idx = " << o.first << "_idx0\n";
                    }
                    do_indent();
                    stream << "infer mport " << o.first << "_rd = " << p.first << "[" << o.first << "_idx], clock\n";
                    do_indent();
                    stream << o.first << ".value <= " << o.first << "_rd.value\n";
                }
            }
        } else {
            do_indent(); stream << s << " <= " << p.first << "\n";
        }
    }

    close_scope(" end of " + c->getModuleName());
    stream << "\n";
}

void CodeGen_FIRRTL_Target::print_io(IO *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";

    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    FIRRTL_Type out_stencil;
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    // generate body
    FIRRTL_Type in_stencil;
    for(auto &p : c->getInputs()) { // only one input
        in_stencil = p.second;
        break;
    }
    do_indent(); stream << "; Parameters:\n";
    if (c->isInputIO()) {
        do_indent(); stream << ";  IO Type= IO_IN\n";
    } else {
        do_indent(); stream << ";  IO Type= IO_OUT\n";
    }
    do_indent(); stream << ";  Type=" << in_stencil.elemType << "\n";
    do_indent(); stream << ";  Bits=" << in_stencil.elemType.bits() << "\n";
    do_indent(); stream << ";  Stencil=";
    vector<int> stencil_size;
    for(const auto &range : in_stencil.bounds) {
        stream << "[" << range.extent << "]";
        const IntImm *int_imm = range.extent.as<IntImm>();
        stencil_size.push_back(int_imm->value);
    }
    stream << "\n";
    do_indent(); stream << ";  Image Size=";
    vector<int> store_extents = c->getStoreExtents();
    vector<int> se_nBits;
    for(const auto &s : store_extents) {
        se_nBits.push_back(std::max((int)std::ceil(std::log2((float)s)), 1));
        stream << "[" << s << "]";
    }
    stream << "\n";
    stream << "\n";

    in_stencil.type = FIRRTL_Type::StencilContainerType::Stencil; // stream to stencil

    map<string, FIRRTL_Type> istreams = c->getInputs();
    string istr;
    for(auto &i : istreams) {
        istr = i.first;
    }
    map<string, FIRRTL_Type> ostreams = c->getOutputs();
    string ostr;
    for(auto &i : ostreams) {
        ostr = i.first;
    }

    // Body of IO
    int store_extents_size = store_extents.size();
    for(int i = 0; i < store_extents_size; i++) {
        do_indent(); stream << "reg counter_" << i << " : UInt<" << se_nBits[i] << ">, clock with : (reset => (reset, UInt<" << se_nBits[i] << ">(0)))\n";
    }

    do_indent();
    stream << "reg valid_d1 : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg " << ostr << "_value : " << print_stencil_type(in_stencil) << ", clock\n";
    do_indent(); stream << "reg started : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg state : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";


    if (c->isInputIO()) {
        do_indent(); stream << ostr << ".value is invalid\n";
        do_indent(); stream << ostr << ".valid is invalid\n";
    } else {
        do_indent(); stream << ostr << ".TDATA is invalid\n";
        do_indent(); stream << ostr << ".TVALID is invalid\n";
    }
    do_indent(); stream << "done_out is invalid\n";


    if (c->isInputIO()) {
        do_indent(); stream << istr << ".TREADY <= UInt<1>(0)\n";
        do_indent(); stream << ostr << ".value <= " << ostr << "_value\n";
        do_indent(); stream << ostr << ".valid <= UInt<1>(0)\n";
    } else {
        do_indent(); stream << istr << ".ready <= UInt<1>(0)\n";
        do_indent(); stream << ostr << ".TDATA <= " << ostr << "_value\n";
        do_indent(); stream << ostr << ".TVALID <= UInt<1>(0)\n";
        do_indent(); stream << ostr << ".TLAST <= UInt<1>(0)\n";
    }

    do_indent(); stream << "done_out <= UInt<1>(0)\n";
    do_indent(); stream << "when start_in :\n";
    open_scope();
    do_indent(); stream << "started <= UInt<1>(1)\n";
    for(int i = 0; i < store_extents_size; i++) {
        do_indent(); stream << "counter_" << i << " <= UInt<" << se_nBits[i] << ">(0)\n";
    }
    do_indent(); stream << "valid_d1 <= UInt<1>(0)\n";
    do_indent(); stream << "state <= UInt<1>(0)\n";
    close_scope("");
    do_indent(); stream << "else when done_out :\n";
    open_scope();
    do_indent(); stream << "started <= UInt<1>(0)\n";
    close_scope("");

    do_indent(); stream << "when started :\n";
    open_scope();

    if (c->isInputIO()) {
        do_indent(); stream << "when " << ostr << ".ready :\n";
    } else {
        do_indent(); stream << "when " << ostr << ".TREADY :\n";
    }
    open_scope();

    // State 0
    do_indent(); stream << "when eq(state, UInt<1>(0)) :\n";
    open_scope();

    if (c->isInputIO()) {
        do_indent(); stream << "when " << istr << ".TVALID :\n";
    } else {
        do_indent(); stream << "when " << istr << ".valid :\n";
    }
    open_scope();

    for(int i = 0 ; i < store_extents_size ; i++) {
        do_indent(); stream << "node counter_" << i << "_is_max = eq(counter_" << i << ", UInt(" << store_extents[i]-stencil_size[i] << "))\n";
        // Note: stencil_size can be bigger than 1. For input IO, store bounds are only availabel in 
        // testbench side through "subimage_to_stream()", so it should be inferred from image size and stencil size.
        do_indent(); stream << "node counter_" << i << "_inc_c = add(counter_" << i << ", UInt(" << stencil_size[i] << "))\n";
        do_indent(); stream << "node counter_" << i << "_inc = tail(counter_" << i << "_inc_c, 1)\n";
        do_indent(); stream << "counter_" << i << " <= counter_" << i << "_inc\n";
        do_indent(); stream << "when counter_" << i << "_is_max :\n";
        open_scope();
        do_indent(); stream << "counter_" << i << " <= UInt<" << se_nBits[i] << ">(0)\n";
    }
    do_indent(); stream << "state <= UInt<1>(1)\n";

    for(int i = store_extents_size-1 ; i >= 0 ; i--) { // reverse order
        close_scope("counter_" + std::to_string(i));
    }

    do_indent(); stream << "valid_d1 <= UInt<1>(1)\n";
    if (c->isInputIO()) {
        do_indent(); stream << ostr << "_value <= " << istr << ".TDATA\n";
        do_indent(); stream << istr << ".TREADY <= UInt<1>(1)\n"; // pop from previous FIFO
        do_indent(); stream << ostr << ".valid <= valid_d1\n";
    } else {
        do_indent(); stream << ostr << "_value <= " << istr << ".value\n";
        do_indent(); stream << istr << ".ready <= UInt<1>(1)\n"; // pop from previous FIFO
        do_indent(); stream << ostr << ".TVALID <= valid_d1\n";
    }

    if (c->isInputIO()) {
        close_scope(istr + ".TVALID");
    } else {
        close_scope(istr + ".valid");
    }

    close_scope("state0");

    // State 1
    do_indent(); stream << "when eq(state, UInt<1>(1)) :\n";
    open_scope();
    if (c->isInputIO()) {
        do_indent(); stream << ostr << ".valid <= valid_d1\n"; // push to next FIFO (when ready)
    } else {
        do_indent(); stream << ostr << ".TVALID <= valid_d1\n"; // push to next FIFO (when ready)
        do_indent(); stream << ostr << ".TLAST <= UInt<1>(1)\n"; // push to next FIFO (when ready)
    }
    do_indent(); stream << "state <= UInt<1>(0)\n";
    do_indent(); stream << "done_out <= UInt<1>(1)\n";
    close_scope("state1");

    if (c->isInputIO()) {
        close_scope(ostr + ".ready");
    } else {
        close_scope(ostr + ".TREADY");
    }

    close_scope("started");

    close_scope(" end of " + c->getModuleName());
    stream << "\n";
}

void CodeGen_FIRRTL_Target::print_fifo(FIFO *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";

    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    // TODO: generate body of the FIFO.
    FIRRTL_Type s;
    for(auto &p : c->getInputs()) { // only one input
        s = p.second;
        break;
    }
    do_indent(); stream << "; Parameters:\n";
    do_indent(); stream << ";  Type=" << s.elemType << "\n";
    do_indent(); stream << ";  Bits=" << s.elemType.bits() << "\n";
    do_indent(); stream << ";  Stencil=";
    for(const auto &range : s.bounds) {
        stream << "[" << range.extent << "]";
    }
    stream << "\n";
    int depth = std::stoi(c->getDepth());
    do_indent(); stream << ";  Depth=" << depth << "\n";
    stream << "\n";
    int nBit = std::max((int)std::ceil(std::log2((float)depth+1)), 1);
    int level_nBit = (int)std::ceil(std::log2((float)depth+2));

    // generate body
    s.type = FIRRTL_Type::StencilContainerType::Stencil;
    do_indent(); stream << "cmem  mem : {value : " << print_stencil_type(s) << "}[" << depth+1 << "]\n";
    do_indent(); stream << "reg  r_wr_ptr : UInt<" << nBit << ">, clock with : (reset => (reset, UInt<" << nBit << ">(0)))\n";
    do_indent(); stream << "reg  r_rd_ptr : UInt<" << nBit << ">, clock with : (reset => (reset, UInt<" << nBit << ">(0)))\n";
    do_indent(); stream << "reg  r_level : UInt<" << level_nBit << ">, clock with : (reset => (reset, UInt<" << level_nBit << ">(0)))\n";
    do_indent(); stream << "wire w_push : UInt<1>\n";
    do_indent(); stream << "wire w_pop : UInt<1>\n";
    do_indent(); stream << "reg  r_empty : UInt<1>, clock with : (reset => (reset, UInt<1>(1)))\n";
    do_indent(); stream << "reg  r_full : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg  r_valid_out : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg  r_data_out : " << print_stencil_type(s) << ", clock\n";
    stream << "\n";

    do_indent(); stream << "w_push <= and(data_in.valid, not(r_full))\n";
    do_indent(); stream << "w_pop  <= and(or(data_out.ready, not(r_valid_out)), not(r_empty))\n";
    stream << "\n";

    do_indent(); stream << "when w_push :\n";
    do_indent(); stream << "  node r_wr_ptr_is_max = eq(r_wr_ptr, UInt<" << nBit << ">(" << depth << "))\n";
    do_indent(); stream << "  node r_wr_ptr_inc_c = add(r_wr_ptr, UInt<1>(1))\n";
    do_indent(); stream << "  node r_wr_ptr_inc = tail(r_wr_ptr_inc_c, 1)\n";
    do_indent(); stream << "  r_wr_ptr <= r_wr_ptr_inc\n";
    do_indent(); stream << "  when r_wr_ptr_is_max :\n";
    do_indent(); stream << "    r_wr_ptr <= UInt<" << nBit << ">(0)\n";
    stream << "\n";

    do_indent(); stream << "when w_pop :\n";
    do_indent(); stream << "  node r_rd_ptr_is_max = eq(r_rd_ptr, UInt<" << nBit << ">(" << depth << "))\n";
    do_indent(); stream << "  node r_rd_ptr_inc_c = add(r_rd_ptr, UInt<1>(1))\n";
    do_indent(); stream << "  node r_rd_ptr_inc = tail(r_rd_ptr_inc_c, 1)\n";
    do_indent(); stream << "  r_rd_ptr <= r_rd_ptr_inc\n";
    do_indent(); stream << "  when r_rd_ptr_is_max :\n";
    do_indent(); stream << "    r_rd_ptr <= UInt<" << nBit << ">(0)\n";
    stream << "\n";

    do_indent(); stream << "when and(w_push, not(w_pop)) :\n";
    do_indent(); stream << "  r_level <= tail(add(r_level, UInt<1>(1)), 1)\n";
    do_indent(); stream << "else when and(not(w_push), w_pop) :\n";
    do_indent(); stream << "  r_level <= tail(asUInt(sub(r_level, UInt<1>(1))), 1)\n";
    stream << "\n";

    do_indent(); stream << "when and(w_push, not(w_pop)) :\n";
    do_indent(); stream << "  r_empty <= UInt<1>(0)\n";
    do_indent(); stream << "else when and(not(w_push), w_pop) :\n";
    do_indent(); stream << "  when eq(r_level, UInt<" << nBit << ">(1)) :\n";
    do_indent(); stream << "    r_empty <= UInt<1>(1)\n";
    do_indent(); stream << "  else :\n";
    do_indent(); stream << "    r_empty <= UInt<1>(0)\n";
    stream << "\n";

    do_indent(); stream << "when and(w_push, not(w_pop)) :\n";
    do_indent(); stream << "  when eq(r_level, UInt<" << nBit << ">(" << depth << ")) :\n";
    do_indent(); stream << "    r_full <= UInt<1>(1)\n";
    do_indent(); stream << "  else :\n";
    do_indent(); stream << "    r_full <= UInt<1>(0)\n";
    do_indent(); stream << "else when and(not(w_push), w_pop) :\n";
    do_indent(); stream << "  r_full <= UInt<1>(0)\n";
    stream << "\n";

    do_indent(); stream << "when w_push :\n";
    do_indent(); stream << "  infer mport mem_wr = mem[r_wr_ptr], clock\n";
    do_indent(); stream << "  mem_wr.value <= data_in.value\n";
    stream << "\n";

    do_indent(); stream << "when w_pop :\n";
    do_indent(); stream << "  infer mport mem_rd = mem[r_rd_ptr], clock\n";
    do_indent(); stream << "  r_data_out <= mem_rd.value\n";
    stream << "\n";

    do_indent(); stream << "data_out.value <= r_data_out\n";
    stream << "\n";

    do_indent(); stream << "when w_pop :\n";
    do_indent(); stream << "  r_valid_out <= UInt<1>(1)\n";
    do_indent(); stream << "else when data_out.ready :\n";
    do_indent(); stream << "  r_valid_out <= UInt<1>(0)\n";
    stream << "\n";

    do_indent(); stream << "data_out.valid <= r_valid_out\n";
    stream << "\n";

    do_indent(); stream << "data_in.ready <= not(r_full)\n";

    close_scope(" end of " + c->getModuleName());
    stream << "\n";
}

void CodeGen_FIRRTL_Target::print_linebuffer(LineBuffer *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";

    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    // TODO: generate body of the Linebuffer.
    string      in_stream;
    FIRRTL_Type in_stencil;
    for(auto &p : c->getInputs()) { // only one input
        in_stream  = p.first;
        in_stencil = p.second;
        break;
    }
    string      out_stream;
    FIRRTL_Type out_stencil;
    for(auto &p : c->getOutputs()) { // only one output
        out_stream  = p.first;
        out_stencil = p.second;
        break;
    }

    int L[4] = {1, 1, 1, 1};
    int inEl[4] = {1, 1, 1, 1};
    int outEl[4] = {1, 1, 1, 1};

    do_indent(); stream << "; Parameters:\n";
    do_indent(); stream << ";  Type=" << in_stencil.elemType << "\n";
    do_indent(); stream << ";  Bits=" << in_stencil.elemType.bits() << "\n";
    do_indent(); stream << ";  Input Stencil=";
    int dim=0;
    for(const auto &range : in_stencil.bounds) {
        stream << "[" << range.extent << "]";
        const IntImm *int_imm = range.extent.as<IntImm>();
        inEl[dim++] = int_imm->value;
    }
    stream << "\n";
    do_indent(); stream << ";  Output Stencil=";
    dim = 0;
    for(const auto &range : out_stencil.bounds) {
        stream << "[" << range.extent << "]";
        const IntImm *int_imm = range.extent.as<IntImm>();
        outEl[dim++] = int_imm->value;
    }
    stream << "\n";
    do_indent(); stream << ";  Image Size=";
    dim = 0;
    for(const auto &s : c->getStoreExtents()) {
        stream << "[" << s << "]";
        L[dim++] = s;
    }
    stream << "\n";

    int nDim = in_stencil.bounds.size();
    string inS = print_type(in_stencil.elemType) + "[" + std::to_string(inEl[0]) + "][" + std::to_string(inEl[1]) + "][" + std::to_string(inEl[2]) + "][" + std::to_string(inEl[3]) + "]";

    // print wrapper
    do_indent(); stream << "clock is invalid\n";
    do_indent(); stream << "reset is invalid\n";
    do_indent(); stream << in_stream << " is invalid\n";
    do_indent(); stream << out_stream << " is invalid\n";
    do_indent(); stream << in_stream << ".ready <= UInt<1>(0)\n";
    do_indent(); stream << "inst LB_" << out_stream << "_" << nDim << "D of LB_" << out_stream << "_" << nDim << "D\n";
    do_indent(); stream << "LB_" << out_stream << "_" << nDim << "D.io is invalid\n";
    do_indent(); stream << "LB_" << out_stream << "_" << nDim << "D.clock <= clock\n";
    do_indent(); stream << "LB_" << out_stream << "_" << nDim << "D.reset <= reset\n";
    do_indent(); stream << "LB_" << out_stream << "_" << nDim << "D.io.in.valid <= UInt<1>(0)\n";
    do_indent(); stream << "wire _inv : {value : " << inS << "}\n";
    do_indent(); stream << "_inv is invalid\n";
    for (int i3=0; i3<inEl[3]; i3++) {
    for (int i2=0; i2<inEl[2]; i2++) {
    for (int i1=0; i1<inEl[1]; i1++) {
    for (int i0=0; i0<inEl[0]; i0++) {
        do_indent(); stream << "LB_" << out_stream << "_" << nDim << "D.io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= _inv.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
    }
    }
    }
    }
    do_indent(); stream << in_stream << ".ready <= UInt<1>(1)\n";
    do_indent(); stream << "when " << in_stream << ".valid :\n";
    do_indent(); stream << "  LB_" << out_stream << "_" << nDim << "D.io.in.valid <= UInt<1>(1)\n";
    for (int i3=0; i3<inEl[3]; i3++) {
    for (int i2=0; i2<inEl[2]; i2++) {
    for (int i1=0; i1<inEl[1]; i1++) {
    for (int i0=0; i0<inEl[0]; i0++) {
        do_indent(); stream << "  LB_" << out_stream << "_" << nDim << "D.io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= " << in_stream << ".value";
        if (nDim == 1) {
            stream << "[" << i0 << "]\n";
        } else if (nDim == 2) {
            stream << "[" << i1 << "][" << i0 << "]\n";
        } else if (nDim == 3) {
            stream << "[" << i2 << "][" << i1 << "][" << i0 << "]\n";
        } else { // if (nDim == 4) 
            stream << "[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
    }
    }
    }
    }
    do_indent(); stream << "  skip\n";
    do_indent(); stream << in_stream << ".ready <= " << "LB_" << out_stream << "_" << nDim << "D.io.in.ready\n";
    for (int i3=0; i3<outEl[3]; i3++) {
    for (int i2=0; i2<outEl[2]; i2++) {
    for (int i1=0; i1<outEl[1]; i1++) {
    for (int i0=0; i0<outEl[0]; i0++) {
        do_indent(); stream << out_stream << ".value";
        if (nDim == 1) {
            stream << "[" << i0 << "]";
        } else if (nDim == 2) {
            stream << "[" << i1 << "][" << i0 << "]";
        } else if (nDim == 3) {
            stream << "[" << i2 << "][" << i1 << "][" << i0 << "]";
        } else { // if (nDim == 4)
            stream << "[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]";
        }
        stream << " <= LB_" << out_stream << "_" << nDim << "D.io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
    }
    }
    }
    }
    do_indent(); stream << out_stream << ".valid <= " << "LB_" << out_stream << "_" << nDim << "D.io.out.valid\n";
    do_indent(); stream << "LB_" << out_stream << "_" << nDim << "D.io.out.ready <= " << out_stream << ".ready\n";
    close_scope(" end of " + c->getModuleName());
    stream << "\n";

    if (nDim==1) { // TODO
        // TODO: assert inEl[1] == outEl[1] == 1
        // TODO: assert inEl[2] == outEl[2] == 1
        // TODO: assert inEl[3] == outEl[3] == 1
        print_linebuffer1D(c->getModuleName(),      // Prefix of submodule(s)
                           L, // Image Width, Height
                           in_stencil.elemType,     // Type
                           inEl,                    // In Stencil Width, Height
                           outEl);                  // Out Stencil Widht, Height
    } else if (nDim==2) { 
        // TODO: assert inEl[2] == outEl[2] == 1
        // TODO: assert inEl[3] == outEl[3] == 1
        print_linebuffer2D(c->getModuleName(),      // Prefix of submodule(s)
                           L, // Image Width, Height
                           in_stencil.elemType,     // Type
                           inEl,                    // In Stencil Width, Height
                           outEl);                  // Out Stencil Widht, Height
    } else if (nDim==3) {
        // TODO: assert inEl[3] == outEl[3] == 1
        print_linebuffer3D(c->getModuleName(),      // Prefix of submodule(s)
                           L, // Image Width, Height
                           in_stencil.elemType,     // Type
                           inEl,                    // In Stencil Width, Height
                           outEl);                  // Out Stencil Widht, Height
    } else { //TODO
        do_indent(); stream << "; 4D linebuffer TODO\n";
    }
}

void CodeGen_FIRRTL_Target::print_linebuffer1D(string name, int L[4], Type t, int inEl[4], int outEl[4])
{
    // TODO: assertion: require(List(inEl.dims).tail == List(outEl.dims).tail, "Except the first dimension, others should match in input and output stencils")
    do_indent();
    stream << "module " << name << "_1D" << " :\n";
    open_scope();

    string inS = print_type(t) + "[" + std::to_string(inEl[0]) + "][" + std::to_string(inEl[1]) + "][" + std::to_string(inEl[2]) + "][" + std::to_string(inEl[3]) + "]";
    string outS = print_type(t) + "[" + std::to_string(outEl[0]) + "][" + std::to_string(outEl[1]) + "][" + std::to_string(outEl[2]) + "][" + std::to_string(outEl[3]) + "]";

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";
    do_indent();
    stream << "output io : {flip in : {flip ready : UInt<1>, valid : UInt<1>, bits : {value : " << inS << "}}, "
           << "out : {flip ready : UInt<1>, valid : UInt<1>, bits : {value : " << outS << "}}}\n";
    stream << "\n";

    do_indent(); stream << "clock is invalid\n";
    do_indent(); stream << "reset is invalid\n";
    do_indent(); stream << "io is invalid\n";

    if (inEl[0] == outEl[0]) {
        do_indent(); stream << "io.out.bits.value <= io.in.bits.value\n";
        do_indent(); stream << "io.in.ready <= io.out.ready\n";
        do_indent(); stream << "io.out.valid <= io.in.valid\n";
    } else {// TODO: } else if (isOutDimDivisibleByIn(0)) {
        int ratio = outEl[0]/inEl[0];
        int bufL0 = std::max(0,ratio - 1);
        int imgL0 = L[0]/inEl[0];
        int nBit_imgL0 = (int)std::ceil(std::log2((float)imgL0));

        do_indent();
        stream << "reg buffer : {value : " << inS << "}[" << bufL0 << "], clock\n";
        do_indent();
        stream << "reg col : UInt<" << nBit_imgL0 << ">, clock with : (reset => (reset, UInt<" << nBit_imgL0 << ">(0)))\n";
        do_indent();
        stream << "wire outStencil : {value : " << outS << "}\n";
        do_indent();
        stream << "outStencil is invalid\n";
        do_indent();
        stream << "io.out.valid <= UInt<1>(0)\n";
        do_indent(); stream << "wire _inv : {value : " << outS << "}\n";
        do_indent(); stream << "_inv is invalid\n";
        for (int i3=0; i3<outEl[3]; i3++) {
        for (int i2=0; i2<outEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<outEl[0]; i0++) {
            do_indent(); stream << "io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= _inv.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.in.ready <= UInt<1>(1)\n";
        do_indent(); stream << "when io.in.valid :\n";
        do_indent(); stream << "  when geq(col, UInt<" << nBit_imgL0 << ">(" << bufL0 << ")) :\n";
        for (int bi=0; bi<bufL0; bi++) {
            int inSliceL0 =  bi * inEl[0];
            for (int i3=0; i3<inEl[3]; i3++) {
            for (int i2=0; i2<inEl[2]; i2++) {
            for (int i1=0; i1<inEl[1]; i1++) {
            for (int i0=0; i0<inEl[0]; i0++) {
                do_indent();
                stream << "    outStencil.value[" << i3 << "][" << i2 << "][" << i1 << "][" << inSliceL0 + i0 << "]"
                          " <= buffer[" << bi << "].value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
            }
            }
            }
            }
        }
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent();
            stream << "    outStencil.value[" << i3 << "][" << i2 << "][" << i1 << "][" << bufL0*inEl[0] + i0 << "]"
                      " <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "    io.out.valid <= UInt<1>(1)\n";
        for (int i3=0; i3<outEl[3]; i3++) {
        for (int i2=0; i2<outEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<outEl[0]; i0++) {
            do_indent();
            stream << "    io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]"
                      " <= outStencil.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "    skip\n";
        do_indent(); stream << "  when io.out.ready :\n";
        for (int bi=0; bi<bufL0-1; bi++) {
            for (int i3=0; i3<inEl[3]; i3++) {
            for (int i2=0; i2<inEl[2]; i2++) {
            for (int i1=0; i1<inEl[1]; i1++) {
            for (int i0=0; i0<inEl[0]; i0++) {
                do_indent();
                stream << "    buffer[" << bi << "].value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]"
                          " <= buffer[" << bi+1 << "].value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
            }
            }
            }
            }
        }
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent();
            stream << "    buffer[" << bufL0-1 << "].value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]"
                      " <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "    node col_is_max = eq(col, UInt<" << nBit_imgL0 << ">(" << imgL0-1 << "))\n";
        do_indent(); stream << "    node col_inc = tail(add(col, UInt<1>(1)), 1)\n";
        do_indent(); stream << "    col <= col_inc\n";
        do_indent(); stream << "    when col_is_max :\n";
        do_indent(); stream << "      col <= UInt<1>(0)\n";
        do_indent(); stream << "      skip\n";
        do_indent(); stream << "    skip\n";
        do_indent(); stream << "  skip\n";
        do_indent(); stream << "io.in.ready <= io.out.ready\n";
    }

    close_scope(" end of " + name + "_1D");
    stream << "\n";
}

void CodeGen_FIRRTL_Target::print_linebuffer2D(string name, int L[4], Type t, int inEl[4], int outEl[4])
{
    // TODO: require(isOutDimDivisibleByIn(1))
    // TODO: require(inEl.dim(2) == outEl.dim(2))
    // TODO: require(inEl.dim(3) == outEl.dim(3))

    do_indent();
    stream << "module " << name << "_2D" << " :\n";
    open_scope();

    string inS = print_type(t) + "[" + std::to_string(inEl[0]) + "][" + std::to_string(inEl[1]) + "][" + std::to_string(inEl[2]) + "][" + std::to_string(inEl[3]) + "]";
    string outS = print_type(t) + "[" + std::to_string(outEl[0]) + "][" + std::to_string(outEl[1]) + "][" + std::to_string(outEl[2]) + "][" + std::to_string(outEl[3]) + "]";
    string l1S = print_type(t) + "[" + std::to_string(inEl[0]) + "][" + std::to_string(outEl[1]) + "][" + std::to_string(outEl[2]) + "][" + std::to_string(outEl[3]) + "]";

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";
    do_indent();
    stream << "output io : {flip in : {flip ready : UInt<1>, valid : UInt<1>, bits : {value : " << inS << "}}, "
           << "out : {flip ready : UInt<1>, valid : UInt<1>, bits : {value : " << outS << "}}}\n";
    stream << "\n";

    do_indent(); stream << "clock is invalid\n";
    do_indent(); stream << "reset is invalid\n";
    do_indent(); stream << "io is invalid\n";

    if ((inEl[0] == outEl[0])&&(inEl[1] == outEl[1])) {
        do_indent(); stream << "io.in.ready <= UInt<1>(0)\n";
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << "io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.in.ready <= io.out.ready\n";
        do_indent(); stream << "io.out.valid <= io.in.valid\n";
    } else { //} else if(isOutDimDivisibleByIn(0) && isOutDimDivisibleByIn(1)) {
        // TODO: require(L0 > inEl.dim(0) && L0 > outEl.dim(0))
        // TODO: require(L0 % inEl.dim(0) == 0)

        int ratio1 = outEl[1]/inEl[1];
        int bufL0 = L[0]/inEl[0];
        int bufL1 = ratio1 - 1;
        int imgL0 = L[0]/inEl[0];
        int imgL1 = L[1]/inEl[1];
        int nBit_imgL0 = (int)std::ceil(std::log2((float)imgL0));
        int nBit_imgL1 = (int)std::ceil(std::log2((float)imgL1));
        int nBit_bufL1 = std::max((int)std::ceil(std::log2((float)bufL1)), 1);
        int nBit_inEl1 = std::max((int)std::ceil(std::log2((float)inEl[1])), 1);
        int nBit_outEl1 = std::max((int)std::ceil(std::log2((float)outEl[1])), 1);

        do_indent(); stream << "io.in.ready <= UInt<1>(0)\n";
        do_indent(); stream << "reg col : UInt<" << nBit_imgL0 << ">, clock with : (reset => (reset, UInt<" << nBit_imgL0 << ">(0)))\n";
        do_indent(); stream << "reg row : UInt<" << nBit_imgL1 << ">, clock with : (reset => (reset, UInt<" << nBit_imgL1 << ">(0)))\n";
        for(int i=0; i<bufL1; i++) {
            do_indent(); stream << "cmem buffer" << i << " : {value : " << inS << "}[" << bufL0 << "]\n";
        }
        if (bufL1!=0) {
            do_indent(); stream << "reg writeIdx1 : UInt<" << nBit_bufL1 << ">, clock with : (reset => (reset, UInt<" << nBit_bufL1 << ">(0)))\n";
        }
        do_indent(); stream << "wire slice : {value : " << l1S << "}\n";
        do_indent(); stream << "slice is invalid\n";
        do_indent(); stream << "inst " << name << "_1D of " << name << "_1D" << "\n";
        do_indent(); stream << name << "_1D.io is invalid\n";
        do_indent(); stream << name << "_1D.clock <= clock\n";
        do_indent(); stream << name << "_1D.reset <= reset\n";
        do_indent(); stream << name << "_1D.io.in.valid <= UInt<1>(0)\n";
        do_indent(); stream << "wire _inv : {value : " << l1S << "}\n";
        do_indent(); stream << "_inv is invalid\n";
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << name << "_1D.io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= _inv.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.in.ready <= UInt<1>(1)\n";
        do_indent(); stream << "when io.in.valid :\n";
        do_indent(); stream << "  when geq(row, UInt<" << nBit_imgL1 << ">(" << bufL1 << ")) :\n";
        for(int l1=0; l1<bufL1; l1++) {
            do_indent(); stream << "    infer mport buffer" << l1 << "_rd = buffer" << l1 << "[col], clock\n";
            do_indent(); stream << "    node inSliceL1s_buffer" << l1 << " = tail(asUInt(sub(UInt<" << nBit_bufL1+1 << ">(" << bufL1+l1 << "), writeIdx1)), 1)\n";
            do_indent(); stream << "    wire inSliceL1_buffer" << l1 << " : UInt<" << nBit_outEl1 << ">\n";
            do_indent(); stream << "    inSliceL1_buffer" << l1 << " is invalid\n";
            do_indent(); stream << "    when geq(inSliceL1s_buffer" << l1 << ", UInt<" << nBit_bufL1+1 << ">(" << bufL1 << ")) :\n";
            do_indent(); stream << "      inSliceL1_buffer" << l1 << " <= tail(asUInt(sub(inSliceL1s_buffer" << l1 << ", UInt<" << nBit_bufL1+1 << ">(" << bufL1 << "))), 1)\n";
            do_indent(); stream << "      skip\n";
            do_indent(); stream << "    else :\n";
            do_indent(); stream << "      inSliceL1_buffer" << l1 << " <= inSliceL1s_buffer" << l1 << "\n";
            do_indent(); stream << "      skip\n";
            if (inEl[1]==1) {
                do_indent(); stream << "    node inSliceL1m_buffer" << l1 << " = inSliceL1_buffer" << l1 << "\n";
            } else {
                do_indent(); stream << "    node inSliceL1m_buffer" << l1 << " = mul(inSliceL1_buffer" << l1 << ", UInt<" << nBit_inEl1 << ">(" << inEl[1] << "))\n";
            }
            for (int i3=0; i3<inEl[3]; i3++) {
            for (int i2=0; i2<inEl[2]; i2++) {
            for (int i1=0; i1<inEl[1]; i1++) {
                if (inEl[1]==1) {
                    do_indent(); stream << "    node inSliceL1ma" << i3 << i2 << i1 << "_buffer" << l1 << " = bits(inSliceL1m_buffer" << l1 << ", " << nBit_outEl1-1 << ", 0)\n";
                } else {
                    do_indent(); stream << "    node inSliceL1ma" << i3 << i2 << i1 << "_buffer" << l1 << " = bits(tail(add(inSliceL1m_buffer" << l1 << ", UInt<" << nBit_outEl1 << ">(" << i1 << ")), 1), " << nBit_outEl1-1 << ", 0)\n";
                }
                for (int i0=0; i0<inEl[0]; i0++) {
                    do_indent(); stream << "    slice.value[" << i3 << "][" << i2 << "][inSliceL1ma" << i3 << i2 << i1 << "_buffer" << l1 << "][" << i0 << "] <=  buffer" << l1 << "_rd.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
                }
            }
            }
            }
        }
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << "    slice.value[" << i3 << "][" << i2 << "][" << bufL1*inEl[1]+i1 << "][" << i0 << "] <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }

        do_indent(); stream << "    " << name << "_1D.io.in.valid <= UInt<1>(1)\n";

        for (int i3=0; i3<outEl[3]; i3++) {
        for (int i2=0; i2<outEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << "    " << name << "_1D.io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= slice.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "    skip\n";

        do_indent(); stream << "  when " << name << "_1D.io.in.ready :\n";
        do_indent(); stream << "    node col_is_max = eq(col, UInt<" << nBit_imgL0 << ">(" << imgL0-1 << "))\n";
        do_indent(); stream << "    node col_inc_c = add(col, UInt<1>(1))\n";
        do_indent(); stream << "    node col_inc = tail(col_inc_c, 1)\n";
        do_indent(); stream << "    col <= col_inc\n";
        do_indent(); stream << "    when col_is_max :\n";
        do_indent(); stream << "      col <= UInt<1>(0)\n";
        do_indent(); stream << "      skip\n";
        do_indent(); stream << "    when col_is_max :\n";
        do_indent(); stream << "      node row_is_max = eq(row, UInt<" << nBit_imgL1 << ">(" << imgL1-1 << "))\n";
        do_indent(); stream << "      node row_inc = tail(add(row, UInt<1>(1)), 1)\n";
        do_indent(); stream << "      row <= row_inc\n";
        do_indent(); stream << "      when row_is_max :\n";
        do_indent(); stream << "        row <= UInt<1>(0)\n";
        do_indent(); stream << "        skip\n";
        if (bufL1!=0) {
            do_indent(); stream << "      node writeIdx_is_max = eq(writeIdx1, UInt<" << nBit_bufL1 << ">(" << bufL1-1 << "))\n";
            do_indent(); stream << "      node writeIdx_inc = tail(add(writeIdx1, UInt<1>(1)), 1)\n";
            do_indent(); stream << "      writeIdx1 <= writeIdx_inc\n";
            do_indent(); stream << "      when writeIdx_is_max :\n";
            do_indent(); stream << "        writeIdx1 <= UInt<" << nBit_bufL1 << ">(0)\n";
        }
        do_indent(); stream << "      skip\n";

        for(int l1=0; l1<bufL1; l1++) {
            do_indent(); stream << "    when eq(UInt<" << nBit_bufL1 << ">(" << l1 << "), writeIdx1) :\n";
            do_indent(); stream << "      infer mport buffer" << l1 << "_wr = buffer" << l1 << "[col], clock\n";
            for (int i3=0; i3<inEl[3]; i3++) {
            for (int i2=0; i2<inEl[2]; i2++) {
            for (int i1=0; i1<inEl[1]; i1++) {
            for (int i0=0; i0<inEl[0]; i0++) {
                do_indent(); stream << "      buffer" << l1 << "_wr.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
            }
            }
            }
            }
            do_indent(); stream << "      skip\n";
        }
        do_indent(); stream << "    skip\n";
        do_indent(); stream << "  skip\n";

        do_indent(); stream << "io.in.ready <= " << name << "_1D.io.in.ready\n";

        for (int i3=0; i3<outEl[3]; i3++) {
        for (int i2=0; i2<outEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<outEl[0]; i0++) {
            do_indent(); stream << "io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "] <= " << name << "_1D.io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }

        do_indent(); stream << "io.out.valid <= " << name << "_1D.io.out.valid\n";
        do_indent(); stream << name << "_1D.io.out.ready <= io.out.ready\n";
    }

    close_scope(" end of " + name + "_2D");
    stream << "\n";

    if ((inEl[0]!=outEl[0]) || (inEl[1]!=outEl[1])) {
        inEl[1] = outEl[1];
        print_linebuffer1D(name, L, t, inEl, outEl);
    }
}

void CodeGen_FIRRTL_Target::print_linebuffer3D(string name, int L[4], Type t, int inEl[4], int outEl[4])
{
    // TODO: require(....
    // TODO: require(inEl.dim(3) == outEl.dim(3) == 1)

    do_indent();
    stream << "module " << name << "_3D" << " :\n";
    open_scope();

    string inS = print_type(t) + "[" + std::to_string(inEl[0]) + "][" + std::to_string(inEl[1]) + "][" + std::to_string(inEl[2]) + "][" + std::to_string(inEl[3]) + "]";
    string outS = print_type(t) + "[" + std::to_string(outEl[0]) + "][" + std::to_string(outEl[1]) + "][" + std::to_string(outEl[2]) + "][" + std::to_string(outEl[3]) + "]";

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";
    do_indent();
    stream << "output io : {flip in : {flip ready : UInt<1>, valid : UInt<1>, bits : {value : " << inS << "}}, "
           << "out : {flip ready : UInt<1>, valid : UInt<1>, bits : {value : " << outS << "}}}\n";
    stream << "\n";

    do_indent(); stream << "clock is invalid\n";
    do_indent(); stream << "reset is invalid\n";
    do_indent(); stream << "io is invalid\n";

    if ((inEl[0] == outEl[0])&&(inEl[0] == L[0])) { // Trivial case, use 2D with stencil transformation.
        string l2S = print_type(t) + "[" + std::to_string(inEl[0]*inEl[1]) + "][" + std::to_string(inEl[2]) + "][" + std::to_string(inEl[3]) + "][1]";
        do_indent(); stream << "io.in.ready <= UInt<1>(0)\n";
        do_indent(); stream << "wire slice : {value : " << l2S << "}\n";
        do_indent(); stream << "slice is invalid\n";
        do_indent(); stream << "inst " << name << "_2D of " << name << "_2D" << "\n";
        do_indent(); stream << name << "_2D.io is invalid\n";
        do_indent(); stream << name << "_2D.clock <= clock\n";
        do_indent(); stream << name << "_2D.reset <= reset\n";
        do_indent(); stream << name << "_2D.io.in.valid <= UInt<1>(0)\n";
        do_indent(); stream << "wire _inv : {value : " << l2S << "}\n";
        do_indent(); stream << "_inv is invalid\n";
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << name << "_2D.io.in.bits.value[0][" << i3 << "][" << i2 << "][" << i1*inEl[0]+i0 << "] ";
            stream << "<= _inv.value[0][" << i3 << "][" << i2 << "][" << i1*inEl[0]+i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.in.ready <= UInt<1>(1)\n";
        do_indent(); stream << "when io.in.valid :\n";
        do_indent(); stream << "  " << name << "_2D.io.in.valid <= UInt<1>(1)\n";
        for (int i3=0; i3<inEl[3]; i3++) { // 3D -> 2D
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << "  " << name << "_2D.io.in.bits.value[0][" << i3 << "][" << i2 << "][" << i1*inEl[0]+i0 << "]";
            stream << " <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "  skip\n";
        do_indent(); stream << "io.in.ready <= " << name << "_2D.io.in.ready\n";
        for (int i3=0; i3<outEl[3]; i3++) { // 2D -> 3D
        for (int i2=0; i2<outEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<outEl[0]; i0++) {
            do_indent(); stream << "io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]";
            stream << " <= " << name << "_2D.io.out.bits.value[0][" << i3 << "][" << i2 << "][" << i0+i1*outEl[0] << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.out.valid <= " << name << "_2D.io.out.valid\n";
        do_indent(); stream << name << "_2D.io.out.ready <= io.out.ready\n";
        close_scope(" end of " + name + "_3D");
        stream << "\n";


        inEl[0] = inEl[0]*inEl[1];
        inEl[1] = inEl[2];
        inEl[2] = inEl[3];
        outEl[0] = outEl[0]*outEl[1];
        outEl[1] = outEl[2];
        outEl[2] = outEl[3];
        L[0] = L[1]*inEl[0];
        L[1] = L[2];
        L[2] = L[3];

        print_linebuffer2D(name, L, t, inEl, outEl);

    } else if ((inEl[2] == outEl[2])&&(inEl[2] == L[2])) { // Trivial case, use 2D with stencil transformation.
        string l2S = print_type(t) + "[" + std::to_string(inEl[0]) + "][" + std::to_string(inEl[1]*inEl[2]) + "][" + std::to_string(inEl[3]) + "][1]";
        do_indent(); stream << "io.in.ready <= UInt<1>(0)\n";
        do_indent(); stream << "wire slice : {value : " << l2S << "}\n";
        do_indent(); stream << "slice is invalid\n";
        do_indent(); stream << "inst " << name << "_2D of " << name << "_2D" << "\n";
        do_indent(); stream << name << "_2D.io is invalid\n";
        do_indent(); stream << name << "_2D.clock <= clock\n";
        do_indent(); stream << name << "_2D.reset <= reset\n";
        do_indent(); stream << name << "_2D.io.in.valid <= UInt<1>(0)\n";
        do_indent(); stream << "wire _inv : {value : " << l2S << "}\n";
        do_indent(); stream << "_inv is invalid\n";
        for (int i3=0; i3<inEl[3]; i3++) {
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << name << "_2D.io.in.bits.value[0][" << i3 << "][" << i2*inEl[1]+i1 << "][" << i0 << "] ";
            stream << "<= _inv.value[0][" << i3 << "][" << i2*inEl[1]+i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.in.ready <= UInt<1>(1)\n";
        do_indent(); stream << "when io.in.valid :\n";
        do_indent(); stream << "  " << name << "_2D.io.in.valid <= UInt<1>(1)\n";
        for (int i3=0; i3<inEl[3]; i3++) { // 3D -> 2D
        for (int i2=0; i2<inEl[2]; i2++) {
        for (int i1=0; i1<inEl[1]; i1++) {
        for (int i0=0; i0<inEl[0]; i0++) {
            do_indent(); stream << "  " << name << "_2D.io.in.bits.value[0][" << i3 << "][" << i2*inEl[1]+i1 << "][" << i0 << "]";
            stream << " <= io.in.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "  skip\n";
        do_indent(); stream << "io.in.ready <= " << name << "_2D.io.in.ready\n";
        for (int i3=0; i3<outEl[3]; i3++) { // 2D -> 3D
        for (int i2=0; i2<outEl[2]; i2++) {
        for (int i1=0; i1<outEl[1]; i1++) {
        for (int i0=0; i0<outEl[0]; i0++) {
            do_indent(); stream << "io.out.bits.value[" << i3 << "][" << i2 << "][" << i1 << "][" << i0 << "]";
            stream << " <= " << name << "_2D.io.out.bits.value[0][" << i3 << "][" << i2+i1*outEl[2] << "][" << i0 << "]\n";
        }
        }
        }
        }
        do_indent(); stream << "io.out.valid <= " << name << "_2D.io.out.valid\n";
        do_indent(); stream << name << "_2D.io.out.ready <= io.out.ready\n";
        close_scope(" end of " + name + "_3D");
        stream << "\n";


        inEl[0] = inEl[0];
        inEl[1] = inEl[1]*inEl[2];
        inEl[2] = inEl[3];
        outEl[0] = outEl[0];
        outEl[1] = outEl[1]*outEl[2];
        outEl[2] = outEl[3];
        L[0] = L[0];
        L[1] = L[1]*inEl[1];
        L[2] = 1;

        print_linebuffer2D(name, L, t, inEl, outEl);

    } else {
        // TODO
        do_indent(); stream << "; Not supported yet\n";
    }

}

void CodeGen_FIRRTL_Target::print_forblock(ForBlock *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";

    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    do_indent(); stream << "; Parameters:\n";
    do_indent(); stream << ";  scan var=";
    for(auto &p : c->getVars()) {
        stream << p << " ";
    }
    stream << "\n";

    do_indent(); stream << ";  min=";
    for(auto &p : c->getMins()) {
        stream << p << " ";
    }
    stream << "\n";

    do_indent(); stream << ";  max=";
    for(auto &p : c->getMaxs()) {
        stream << p << " ";
    }
    stream << "\n";

    do_indent(); stream << ";  stencil var=";
    for(auto &p : c->getStencilVars()) {
        stream << p << " ";
    }
    stream << "\n";

    do_indent(); stream << ";  stencil min=";
    for(auto &p : c->getStencilMins()) {
        stream << p << " ";
    }
    stream << "\n";

    do_indent(); stream << ";  stencil max=";
    for(auto &p : c->getStencilMaxs()) {
        stream << p << " ";
    }
    stream << "\n";

    stream << "\n";

    // Body of ForBlock
    string         out_stream;
    for(auto &p : c->getOutputs()) { // only one output
        out_stream  = p.first;
        break;
    }

    vector<string> vars = c->getVars();
    vector<int>    mins = c->getMins(); // always 0.
    vector<int>    maxs = c->getMaxs();
    vector<int>    maxs_nBits;
    for(auto &p : maxs) {
        maxs_nBits.push_back((int)std::ceil(std::log2((float)p+1))+1);
        // Use +1 bit to prevent becoming minus value when converted to integer.
    }
    vector<string> stencil_vars = c->getStencilVars();
    vector<int>    stencil_mins = c->getStencilMins();
    vector<int>    stencil_maxs = c->getStencilMaxs();
    vector<int>    stencil_nBits;
    for(auto &p : stencil_maxs) {
        stencil_nBits.push_back((int)std::ceil(std::log2((float)p+1))+1);
        // Use +1 bit to prevent becoming minus value when typed cased.
    }

    int ppdepth = c->getPipelineDepth(); // Always 1 for now
    for(unsigned i = 0 ; i < vars.size() ; i++) {
        do_indent();
        // Let's use 32-bit integer so that its behavior is matching with HLS C.
        stream << "reg " << vars[i] << " : SInt<32>, clock with : (reset => (reset, SInt<32>(" << mins[i] << ")))\n";
    }
    for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
        do_indent();
        // Let's use 32-bit integer so that its behavior is matching with HLS C.
        stream << "reg " << stencil_vars[i] << " : SInt<32>, clock with : (reset => (reset, SInt<32>(" << stencil_mins[i] << ")))\n";
        for(int j = 0 ; j < ppdepth ; j++) { // for pipeline forwarding
            do_indent();
            stream << "reg " << stencil_vars[i] << "_d" << (j+1) << " : SInt<32>, clock with : (reset => (reset, SInt<32>(" << stencil_mins[i] << ")))\n";
        }
    }
    for(int j = 0 ; j < ppdepth ; j++) { // for pipeline forwarding
        do_indent();
        stream << "reg valid_d" << (j+1) << " : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    }
    for(int j = 0 ; j < ppdepth ; j++) { // for pipeline forwarding
        do_indent();
        stream << "reg is_last_stencil_d" << (j+1) << " : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    }
    for(auto &p : c->getWires()) {
        do_indent();
        stream << "wire " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    for(auto &p : c->getRegs()) {
        do_indent();
        stream << "reg " << p.first << " : " << print_stencil_type(p.second) << ", clock\n";
    }

    do_indent(); stream << "reg started : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "reg state : UInt<2>, clock with : (reset => (reset, UInt<2>(0)))\n";
    do_indent(); stream << "reg is_last_stencil : UInt<1>, clock with : (reset => (reset, UInt<1>(0)))\n";
    do_indent(); stream << "wire run_step : UInt<1>\n";

    do_indent(); stream << out_stream << ".value is invalid\n";
    do_indent(); stream << out_stream << ".valid is invalid\n";
    for(auto &p : c->getWires()) {
        do_indent();
        stream << p.first << " is invalid\n";
    }
    for(auto &p : c->getInPorts()) {
        if (p.second.type == FIRRTL_Type::StencilContainerType::MemRd) {
            do_indent();
            stream << p.first << ".addr is invalid\n";
        }
    }

    do_indent(); stream << "done_out is invalid\n";
    do_indent(); stream << "run_step is invalid\n";

    for(auto &p : c->getInputs()) {
        do_indent();
        stream << p.first << ".ready <= UInt<1>(0)\n";
    }

    do_indent(); stream << out_stream << ".valid <= UInt<1>(0)\n";

    do_indent(); stream << "done_out <= UInt<1>(0)\n";
    do_indent(); stream << "run_step <= UInt<1>(0)\n";
    do_indent(); stream << "when start_in :\n";
    open_scope();
    do_indent(); stream << "started <= UInt<1>(1)\n";
    for(unsigned i = 0 ; i < vars.size() ; i++) {
        do_indent();
        stream << vars[i] << " <= SInt<32>(" << mins[i] << ")\n";
    }
    for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
        do_indent();
        stream << stencil_vars[i] << " <= SInt<32>(" << stencil_mins[i] << ")\n";
        for(int j = 0 ; j < ppdepth ; j++) {
            do_indent();
            stream << stencil_vars[i] << "_d" << (j+1) << " <= SInt<32>(" << stencil_mins[i] << ")\n";
        }
    }
    for(int j = 0 ; j < ppdepth ; j++) {
        do_indent(); stream << "valid_d" << (j+1) << " <= UInt<1>(0)\n";
    }
    do_indent(); stream << "is_last_stencil <= UInt<1>(0)\n";
    for(int j = 0 ; j < ppdepth ; j++) {
        do_indent(); stream << "is_last_stencil_d" << (j+1) << " <= UInt<1>(0)\n";
    }
    do_indent(); stream << "state <= UInt<2>(0)\n";
    close_scope("");
    do_indent(); stream << "else when done_out :\n";
    open_scope();
    do_indent(); stream << "started <= UInt<1>(0)\n";
    close_scope("");

    do_indent(); stream << "when started :\n";
    open_scope();

    do_indent();
    stream << "when " << out_stream << ".ready :\n";
    open_scope();

    // Print FSM
    // Case 1, When there is no Stencil Var.
    //    S0 -> S0 -> ... -> S2
    // Case 2, When there is Stencil Var and extent of it is equal to 1:
    //    S0 -> S0 -> ... -> S2
    // Case 3, When there is Stencil Var and extent of it is equal or larger than 2:
    //    S0 -> S1 -> S1 ... -> S0 -> S1 -> S1 ... -> S2
    bool is_stencil_loop        =(stencil_vars.size()==1); // TODO stencil_vars.size() > 1
    bool is_stencil_extent_eq1  = false;
    if (is_stencil_loop) {
        is_stencil_extent_eq1 = (stencil_maxs[0]==stencil_mins[0]); // TODO test it!
    }
    bool no_state1              = !is_stencil_loop || is_stencil_extent_eq1;

    // State 0
    do_indent(); stream << "when eq(state, UInt<2>(0)) :\n";
    open_scope();

    for(auto &p : c->getInputs()) {
        do_indent(); stream << "when " << p.first << ".valid :\n";
        open_scope();
    }

    for(int i = vars.size()-1 ; i >= 0 ; i--) { // reverse order
        do_indent(); stream << "node " << vars[i] << "_is_max = eq(" << vars[i] << ", SInt<32>(" << maxs[i] << "))\n";
        do_indent(); stream << "node " << vars[i] << "_inc_c = add(" << vars[i] << ", SInt(1))\n";
        do_indent(); stream << "node " << vars[i] << "_inc = asSInt(tail(" << vars[i] << "_inc_c, 1))\n";
        do_indent(); stream << vars[i] << " <= " << vars[i] << "_inc\n";
        do_indent(); stream << "when " << vars[i] << "_is_max :\n";
        open_scope();
        do_indent(); stream << vars[i] << " <= SInt<32>(" << mins[i] << ")\n";
    }
    do_indent(); stream << "is_last_stencil <= UInt<1>(1)\n";
    if (no_state1) {
        do_indent(); stream << "state <= UInt<2>(2)\n"; // go to state2 directly
    }

    for(unsigned i = 0 ; i < vars.size() ; i++) {
        close_scope(vars[i]);
    }

    do_indent(); stream << "run_step <= UInt<1>(1)\n"; // move pipeline one step forward (when ready&valid)
    if (!no_state1) { // go to state1 for iteration
        do_indent(); stream << "state <= UInt<2>(1)\n";
        do_indent(); stream << stencil_vars[stencil_vars.size()-1] << " <= SInt(1)\n"; // increase last stencil var
    } else {
        for(auto &p : c->getInputs()) {
            do_indent(); stream << p.first << ".ready <= UInt<1>(1)\n"; // pop from previous FIFO
        }
        do_indent(); stream << "valid_d1 <= UInt<1>(1)\n";
        do_indent(); stream << out_stream << ".valid <= valid_d" << ppdepth << "\n"; // push to next FIFO (when ready&valid)
    }

    for(auto &p : c->getInputs()) {
        close_scope(p.first + ".valid");
    }

    close_scope("state0");

    // State 1
    if (!no_state1) {
        do_indent(); stream << "when eq(state, UInt<2>(1)) :\n";
        open_scope();
        do_indent(); stream << "run_step <= UInt<1>(1)\n"; // move pipeline one step forward (when ready&valid)

        for(int i = stencil_vars.size()-1 ; i >= 0 ; i--) { // reverse order
            do_indent(); stream << "node " << stencil_vars[i] << "_is_max = eq(" << stencil_vars[i] << ", SInt<32>(" << stencil_maxs[i] << "))\n";
            do_indent(); stream << "node " << stencil_vars[i] << "_inc_c = add(" << stencil_vars[i] << ", SInt(1))\n";
            do_indent(); stream << "node " << stencil_vars[i] << "_inc = asSInt(tail(" << stencil_vars[i] << "_inc_c, 1))\n";
            do_indent(); stream << stencil_vars[i] << " <= " << stencil_vars[i] << "_inc\n";
            do_indent(); stream << "when " << stencil_vars[i] << "_is_max :\n";
            open_scope();
            do_indent(); stream << stencil_vars[i] << " <= SInt<32>(" << stencil_mins[i] << ")\n";
        }
        for(auto &p : c->getInputs()) { // pop from previous FIFO
            do_indent(); stream << p.first << ".ready <= UInt<1>(1)\n";
        }
        do_indent(); stream << "when is_last_stencil :\n";
        do_indent(); stream << "  state <= UInt<2>(2)\n";
        do_indent(); stream << "  skip\n";
        do_indent(); stream << "else :\n";
        do_indent(); stream << "  state <= UInt<2>(0)\n";
        do_indent(); stream << "  skip\n";
        for(int i = stencil_vars.size()-1 ; i >= 0 ; i--) { // reverse order
            close_scope(stencil_vars[i]);
        }

        close_scope("state1");
    }

    // State 2
    do_indent(); stream << "when eq(state, UInt<2>(2)) :\n";
    open_scope();
    do_indent(); stream << "run_step <= UInt<1>(1)\n"; // move pipeline one step forward (when ready&valid)
    if (no_state1) {
        do_indent(); stream << out_stream << ".valid <= valid_d" << ppdepth << "\n"; // push to next FIFO (when ready)
    }
    do_indent(); stream << "when is_last_stencil_d" << ppdepth << " :\n";
    open_scope();
    for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
        do_indent();
        stream << "when eq(" << stencil_vars[i] << "_d" << ppdepth << ", SInt<32>(" << stencil_maxs[i] << ")) :\n";
        open_scope();
    }
    do_indent(); stream << "state <= UInt<2>(0)\n";
    do_indent(); stream << "done_out <= UInt<1>(1)\n";
    for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
        close_scope(stencil_vars[i]);
    }
    close_scope("is_last_stencil");
    close_scope("state2");

    // forwarding through pipeline
    for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
        for(int j = 0 ; j < ppdepth ; j++) {
            do_indent();
            stream << stencil_vars[i] << "_d" << (j+1) << " <= " << stencil_vars[i] << "\n";
        }
    }
    do_indent(); stream << "is_last_stencil_d1 <= is_last_stencil\n";
    for(int j = 1 ; j < ppdepth ; j++) {
        do_indent();
        stream << "is_last_stencil_d" << (j+1) << " <= is_last_stencil_d" << j << "\n";
    }
    for(int j = 1 ; j < ppdepth ; j++) {
        do_indent();
        stream << "valid_d" << (j+1) << " <= valid_d" << j << "\n";
    }

    if (!no_state1) { // if there is stencil variable(s) max value is bigger than 0, generate output valid based on the stencil counter.
        for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
            do_indent();
            stream << "when eq(" << stencil_vars[i] << "_d" << ppdepth << ", SInt<32>(" << stencil_maxs[i] << ")) :\n";
            open_scope();
        }

        do_indent();
        stream << out_stream << ".valid <= UInt<1>(1)\n";

        for(unsigned i = 0 ; i < stencil_vars.size() ; i++) {
            close_scope(stencil_vars[i] + "_d" + std::to_string(ppdepth));
        }
    }

    close_scope(out_stream + ".ready");

    close_scope("started");

    do_indent(); stream << "when run_step :\n";
    open_scope();

    // printing out oss_body of the ForBlock. (read_stream, computing stage, write_stream)
    for(auto &p : c->print_body()) {
        do_indent(); stream << p << "\n";
    }

    close_scope("run_step");

    close_scope(" end of " + c->getModuleName());
    stream << "\n";
}

void CodeGen_FIRRTL_Target::print_dispatch(Dispatch *c)
{
    do_indent();
    stream << "module " << c->getModuleName() << " :\n";
    open_scope();

    // Print ports.
    do_indent();
    stream << "input clock : Clock\n";
    do_indent();
    stream << "input reset : UInt<1>\n";

    for(auto &p : c->getInPorts()) {
        do_indent();
        stream << "input " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    for(auto &p : c->getOutPorts()) {
        do_indent();
        stream << "output " << p.first << " : " << print_stencil_type(p.second) << "\n";
    }
    stream << "\n";

    // TODO: generate body of the Linebuffer.
    FIRRTL_Type in_stencil;
    string in_name;
    for(auto &p : c->getInputs()) { // only one input
        in_stencil = p.second;
        in_name = p.first;
        break;
    }
    in_stencil.type = FIRRTL_Type::StencilContainerType::Stencil; // stream -> stencil
    do_indent(); stream << "; Parameters:\n";
    do_indent(); stream << ";  Type=" << in_stencil.elemType << "\n";
    do_indent(); stream << ";  Bits=" << in_stencil.elemType.bits() << "\n";
    do_indent(); stream << ";  Stencil=";
    for(const auto &range : in_stencil.bounds) {
        stream << "[" << range.extent << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  stencil_sizes=";
    for(const auto &s : c->getStencilSizes()) {
        stream << "[" << s << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  stencil_steps=";
    for(const auto &s : c->getStencilSteps()) {
        stream << "[" << s << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  store_extents=";
    for(const auto &s : c->getStoreExtents()) {
        stream << "[" << s << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  fifo_depth   =";
    for(const auto &s : c->getConsumerFifoDepths()) {
        stream << "[" << s << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  consumer_offset   =";
    for(const auto &s : c->getConsumerOffsets()) {
        stream << "[";
        for(const auto &b : s) {
            stream << " " << b;
        }
        stream << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  consumer_extent   =";
    for(const auto &s : c->getConsumerExtents()) {
        stream << "[";
        for(const auto &b : s) {
            stream << " " << b;
        }
        stream << "]";
    }
    stream << "\n";
    do_indent(); stream << ";  consumer   =";
    vector<string> consumer_names;
    for(const auto &s : c->getOutputs()) {
        stream << s.first << " ";
        consumer_names.push_back(s.first);
    }
    stream << "\n";
    stream << "\n";

    // body
    vector<int> stencil_sizes = c->getStencilSizes();
    vector<int> stencil_steps = c->getStencilSteps();
    vector<int> store_extents = c->getStoreExtents();
    vector<int> store_nBits;
    for(unsigned i=0 ; i<store_extents.size() ; i++ ) {
        int nBit = std::max((int)std::ceil(std::log2((float)store_extents[i])), 1);
        store_nBits.push_back(nBit);
    }

    int num_of_dimensions = stencil_sizes.size();

    vector<int> consumer_fifo_depths = c->getConsumerFifoDepths();
    vector<vector<int> > consumer_offsets = c->getConsumerOffsets();
    vector<vector<int> > consumer_extents = c->getConsumerExtents();
    int num_of_consumers = consumer_fifo_depths.size();

    do_indent(); stream << "clock is invalid\n";
    do_indent(); stream << "reset is invalid\n";
    do_indent();
    stream << in_name << " is invalid\n";
    for(auto &p : consumer_names ) {
        do_indent();
        stream << p << " is invalid\n";
    }
    do_indent(); stream << "done_out is invalid\n";
    for(int i=0 ; i < num_of_dimensions ; i++) {
        do_indent();
        stream << "reg counter" << i << " : UInt<" << store_nBits[i] << ">, clock with : (reset => (reset, UInt<" << store_nBits[i] << ">(0)))\n";
    }
    for(auto &p : c->getOutputs()) {
        do_indent();
        stream << p.first << ".valid <= UInt<1>(0)\n";
        do_indent();
        stream << "wire " << p.first << "_inv : " << print_stencil_type(in_stencil) << "\n";
        do_indent();
        stream << p.first << "_inv is invalid\n";
        do_indent();
        stream << p.first << ".value <= " << p.first << "_inv\n";
    }

    do_indent();
    stream << in_name << ".ready <= UInt<1>(0)\n";
    do_indent();
    stream << "done_out <= UInt<1>(0)\n";
    for(int i=0 ; i < num_of_consumers ; i++) {
        internal_assert(num_of_dimensions > 1);
        for(int j=0 ; j < num_of_dimensions ; j++) {
            int lb = consumer_offsets[i][j];
            int ub = consumer_offsets[i][j] + consumer_extents[i][j] - stencil_sizes[j];
            do_indent(); stream << "node c" << i << "d" << j << "lb = geq(counter" << j << ", UInt<" << store_nBits[j] << ">(" << lb << "))\n";
            do_indent(); stream << "node c" << i << "d" << j << "ub = leq(counter" << j << ", UInt<" << store_nBits[j] << ">(" << ub << "))\n";
            do_indent(); stream << "node c" << i << "d" << j << "b = and(c" << i << "d" << j << "lb, c" << i << "d" << j << "ub)\n";
            if (j > 0) {
                do_indent(); stream << "node c" << i << "d" << j << " = and(c" << i << "d" << j << "b, c" << i << "d" << j-1 << "b)\n";
            }
        }
        do_indent(); stream << "node c" << i << "r = and(c" << i << "d" << (num_of_dimensions-1) << ", " << consumer_names[i] << ".ready)\n";
        do_indent(); stream << "node c" << i << " = or(c" << i << "r, not(c" << i << "d" << (num_of_dimensions-1) << "))\n";
    }
    do_indent(); stream << "when " << in_name << ".valid :\n";
    open_scope();
    do_indent(); stream << "node allOutReady = ";
    for(int i=0 ; i < num_of_consumers ; i++) {
        if (i < (num_of_consumers-1) ) {
            stream << "and(c" << i << ", ";
        } else {
            stream << "c" << i;
        }
    }
    for(int i=0 ; i < num_of_consumers ; i++) {
        if (i < (num_of_consumers-1) ) {
            stream << ")";
        } else {
            stream << "\n";
        }
    }
    do_indent(); stream << "when allOutReady :\n";
    open_scope();
    do_indent(); stream << in_name << ".ready <= UInt<1>(1)\n";
    for(int i=0 ; i < num_of_consumers ; i++) {
        do_indent(); stream << "when c" << i << "r :\n";
        open_scope();
        do_indent(); stream << consumer_names[i] << ".valid <= UInt<1>(1)\n";
        do_indent(); stream << consumer_names[i] << ".value <= " << in_name << ".value\n";
        close_scope("");
    }

    for(int i=0 ; i < num_of_dimensions ; i++) {
        if (i > 0) {
            do_indent(); stream << "when counter" << i-1 << "_is_max :\n";
            open_scope();
        }
        int max = store_extents[i] - stencil_sizes[i];
        int step = stencil_steps[i];
        do_indent(); stream << "node counter" << i << "_is_max = eq(counter" << i << ", UInt(" << max << "))\n";
        do_indent(); stream << "node counter" << i << "_inc_c = add(counter" << i << ", UInt(" << step << "))\n";
        do_indent(); stream << "node counter" << i << "_inc = tail(counter" << i << "_inc_c, 1)\n";
        do_indent(); stream << "counter" << i << " <= counter" << i << "_inc\n";
        do_indent(); stream << "when counter" << i << "_is_max :\n";
        do_indent(); stream << "  counter" << i << " <= UInt<1>(0)\n";
        if (i == (num_of_dimensions-1)) { // last one
            do_indent(); stream << "  done_out <= UInt<1>(1)\n";
        }
        do_indent(); stream << "  skip\n";
    }

    for(int i = num_of_dimensions - 2 ; i >= 0; i--) {
        close_scope("counter" + std::to_string(i));
    }

    close_scope("allOutReady");

    do_indent(); stream << "else :\n";

    open_scope();
    do_indent(); stream << in_name << ".ready <= UInt<1>(0)\n";
    close_scope("");

    close_scope(in_name + ".valid");

    close_scope(" end of " + c->getModuleName());
    stream << "\n";
}

void CodeGen_FIRRTL_Target::open_scope()
{
    //cache.clear();
    indent += 2;
}

void CodeGen_FIRRTL_Target::close_scope(const std::string &comment)
{
    do_indent(); stream << "skip ; " << comment << "\n";
    //cache.clear();
    indent -= 2;
}

void CodeGen_FIRRTL_Target::visit(const Variable *op) {
    id = print_name(op->name);
}

void CodeGen_FIRRTL_Target::visit(const Cast *op)
{
    // Solution to match with C type conversion rule:
    //   Perform Bit-width extension/shrink before type conversion.

    int lhs_bits = (op->type).bits();
    int rhs_bits = (op->value).type().bits();

    if (lhs_bits==rhs_bits) { // simplification
        if ((op->type).is_int()) {
            print_assignment(op->type, "asSInt(" + print_expr(op->value) + ")");
        } else {
            print_assignment(op->type, "asUInt(" + print_expr(op->value) + ")");
        }
    } else if (lhs_bits>rhs_bits) { // narrow to wider
        string b = std::to_string(lhs_bits); // pad() doesn't change type.
        print_assignment(op->type, "pad(" + print_expr(op->value) + ", " + b + ")");
    } else {
        string b = std::to_string(lhs_bits-1); // wide to narrower
        if ((op->type).is_int()) { // bits() result is always unsigned.
            print_assignment(op->type, "asSInt(bits(" + print_expr(op->value) + ", " + b + ", 0))");
        } else {
            print_assignment(op->type, "bits(" + print_expr(op->value) + ", " + b + ", 0)");
        }
    }
}

void CodeGen_FIRRTL_Target::visit_uniop(Type t, Expr a, const char * op) {
    string sa = print_expr(a);
    string sop(op);
    print_assignment(t, sop + "(" + sa + ")");
}

void CodeGen_FIRRTL_Target::visit_binop(Type t, Expr a, Expr b, const char * op) {
    string sa = print_expr(a);
    string sb = print_expr(b);
    string sop(op);
    print_assignment(t, sop + "(" + sa + ", " + sb + ")");
}

void CodeGen_FIRRTL_Target::visit(const Add *op) {
    ostringstream oss;
    if ((op->type).is_int()) {
        oss << "asSInt("; // tail() makes everything unsigned. convert back.
    }
    oss << "tail(add(" << print_expr(op->a) << ", " << print_expr(op->b) << "), 1)";
    if ((op->type).is_int()) {
        oss << ")";
    }
    print_assignment(op->type, oss.str());
}

void CodeGen_FIRRTL_Target::visit(const Sub *op) {
    //visit_binop(op->type, op->a, op->b, "sub");
    ostringstream oss;
    if ((op->type).is_int()) { // tail() makes everything unsigned. convert back.
        oss << "asSInt(";
    }
    oss << "tail(sub(" << print_expr(op->a) << ", " << print_expr(op->b) << "), 1)";
    if ((op->type).is_int()) {
        oss << ")";
    }
    print_assignment(op->type, oss.str());
}

void CodeGen_FIRRTL_Target::visit(const Mul *op) {
    ostringstream oss;
    //visit_binop(op->type, op->a, op->b, "mul");
    int bits = op->type.bits();
    if ((op->type).is_int()) { // bits() makes everything unsigned. convert back.
        oss << "asSInt(";
    }
    oss << "bits(mul(" << print_expr(op->a) << ", " << print_expr(op->b) << "), " << bits-1 << ", 0)";
    if ((op->type).is_int()) {
        oss << ")";
    }
    print_assignment(op->type, oss.str());
}

void CodeGen_FIRRTL_Target::visit(const Div *op) {
    int bits;
    if (is_const_power_of_two_integer(op->b, &bits)) {
        ostringstream oss;
        oss << "shr(" << print_expr(op->a) << ", " << bits << ")";
        print_assignment(op->type, oss.str());
    } else if (op->type.is_int()) {
        print_expr(lower_euclidean_div(op->a, op->b));
    } else {
        visit_binop(op->type, op->a, op->b, "div");
    }
}

void CodeGen_FIRRTL_Target::visit(const Mod *op) {
    int bits;
    if (is_const_power_of_two_integer(op->b, &bits)) {
        ostringstream oss;
        if ((op->type).is_int()) {
            oss << "asSInt(";
        }
        oss << "and(" << print_expr(op->a) << ", UInt<" << (op->type).bits() << ">(" << ((1 << bits)-1) << "))";
        if ((op->type).is_int()) {
            oss << ")";
        }
        print_assignment(op->type, oss.str());
    } else if (op->type.is_int()) {
        print_expr(lower_euclidean_mod(op->a, op->b));
    } else {
        visit_binop(op->type, op->a, op->b, "rem");
    }
}

void CodeGen_FIRRTL_Target::visit(const Max *op)
{
    Expr cond = op->a > op->b;
    Expr true_value = op->a;
    Expr false_value = op->b;
    Expr new_expr = Select::make(cond, true_value, false_value);
    print(new_expr);
}

void CodeGen_FIRRTL_Target::visit(const Min *op)
{
    Expr cond = op->a < op->b;
    Expr true_value = op->a;
    Expr false_value = op->b;
    Expr new_expr = Select::make(cond, true_value, false_value);
    print(new_expr);
}

void CodeGen_FIRRTL_Target::visit(const EQ *op)
{
    visit_binop(op->type, op->a, op->b, "eq");
}

void CodeGen_FIRRTL_Target::visit(const NE *op)
{
    visit_binop(op->type, op->a, op->b, "neq");
}

void CodeGen_FIRRTL_Target::visit(const LT *op)
{
    visit_binop(op->type, op->a, op->b, "lt");
}

void CodeGen_FIRRTL_Target::visit(const LE *op)
{
    visit_binop(op->type, op->a, op->b, "leq");
}

void CodeGen_FIRRTL_Target::visit(const GT *op)
{
    visit_binop(op->type, op->a, op->b, "gt");
}

void CodeGen_FIRRTL_Target::visit(const GE *op)
{
    visit_binop(op->type, op->a, op->b, "geq");
}

void CodeGen_FIRRTL_Target::visit(const And *op)
{
    Type t = UInt((op->type).bits());
    visit_binop(t, op->a, op->b, "and");
}

void CodeGen_FIRRTL_Target::visit(const Or *op)
{
    Type t = UInt((op->type).bits());
    visit_binop(t, op->a, op->b, "or");
}

void CodeGen_FIRRTL_Target::visit(const Not *op)
{
    print_assignment(op->type, "not(" + print_expr(op->a) + ")");
}

void CodeGen_FIRRTL_Target::visit(const IntImm *op) {
    print_assignment(op->type, print_type(op->type) + "(" + std::to_string(op->value) + ")");
}

void CodeGen_FIRRTL_Target::visit(const UIntImm *op) {
    print_assignment(op->type, print_type(op->type) + "(" + std::to_string(op->value) + ")");
}

void CodeGen_FIRRTL_Target::visit(const StringImm *op)
{
    ostringstream oss;
    oss << Expr(op);
    id = oss.str();
}

// NaN is the only float/double for which this is true... and
// surprisingly, there doesn't seem to be a portable isnan function
// (dsharlet).
template <typename T>
static bool isnan(T x) { return x != x; }

template <typename T>
static bool isinf(T x)
{
    return std::numeric_limits<T>::has_infinity && (
        x == std::numeric_limits<T>::infinity() ||
        x == -std::numeric_limits<T>::infinity());
}

void CodeGen_FIRRTL_Target::visit(const FloatImm *op)
{
    internal_assert(true) << "Not support floating yet..\n";
}

void CodeGen_FIRRTL_Target::visit(const Call *op)
{
    FIRRTL_Type wire_1bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(1),Region(),0,{}};
    FIRRTL_Type wire_16bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(16),Region(),0,{}};

    if (op->is_intrinsic(Call::bitwise_and)) {
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        Type t = UInt((op->type).bits());
        visit_binop(t, a, b, "and");
    } else if (op->is_intrinsic(Call::bitwise_or)) {
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        Type t = UInt((op->type).bits());
        visit_binop(t, a, b, "or");
    } else if (op->is_intrinsic(Call::bitwise_xor)) {
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        Type t = UInt((op->type).bits());
        visit_binop(t, a, b, "xor");
    } else if (op->is_intrinsic(Call::bitwise_not)) {
        internal_assert(op->args.size() == 1);
        Expr a = op->args[0];
        visit_uniop(op->type, a, "not");
    } else if (op->is_intrinsic(Call::reinterpret)) {
        ostringstream rhs;
        internal_assert(op->args.size() == 1);
        Expr a = op->args[0];
        Expr cast_a = cast(op->type, a);
        rhs << print_expr(cast_a);
        print_assignment(op->type, rhs.str());
    } else if (op->is_intrinsic(Call::shift_left)) {
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        const UIntImm *b_imm = b.as<UIntImm>();
        if (b_imm) { // Constant shift, use shl
            ostringstream rhs;
            rhs << "shl(" << print_expr(a) << ", " << b_imm->value << ")";
            print_assignment(op->type, rhs.str());
        } else {
            Type t = UInt(8); // Workaround for the limit: dshl(e, n), n should be 19(or 20) bit or less. 8 might be enough.
            Expr cast_b = cast(t, b);
            visit_binop(op->type, a, cast_b, "dshl");
        }
    } else if (op->is_intrinsic(Call::shift_right)) {
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        const UIntImm *b_imm = b.as<UIntImm>();
        if (b_imm) { // Constant shift, use shr
            ostringstream rhs;
            rhs << "shr(" << print_expr(a) << ", " << std::to_string(b_imm->value) << ")";
            print_assignment(op->type, rhs.str());
        } else {
            Type t = UInt(op->type.bits());
            Expr cast_b = cast(t, b);
            visit_binop(op->type, a, cast_b, "dshr");
        }
    } else if (op->is_intrinsic(Call::lerp)) {
        internal_error << "Call::lerp. What is this? Do we need to support?\n"; // TODO: Do we need this?
    } else if (op->is_intrinsic(Call::absd)) {
        ostringstream rhs;
        internal_assert(op->args.size() == 2);
        Expr a = op->args[0];
        Expr b = op->args[1];
        Expr e = select(a < b, b - a, a - b);
        rhs << print_expr(e);
        print_assignment(op->type, rhs.str());
    } else if (op->is_intrinsic(Call::abs)) {
        ostringstream rhs;
        internal_assert(op->args.size() == 1);
        Expr a0 = op->args[0];
        rhs << print_expr(cast(op->type, select(a0 > 0, a0, -a0)));
        Type t = UInt((op->type).bits());
        print_assignment(t, rhs.str());
    } else if (op->is_intrinsic(Call::div_round_to_zero)) {
        Expr a = op->args[0];
        Expr b = op->args[1];
        visit_binop(op->type, a, b, "div");
    } else if (op->is_intrinsic(Call::mod_round_to_zero)) {
        Expr a = op->args[0];
        Expr b = op->args[1];
        visit_binop(op->type, a, b, "rem");
    } else if(op->name == "linebuffer") {
        const Variable *input = op->args[0].as<Variable>();
        const Variable *output = op->args[1].as<Variable>();
        string inputname  = print_name(input->name);
        string outputname = print_name(output->name);
        FIRRTL_Type in_stype = top->getWire("wire_" + inputname); // get stencil type
        FIRRTL_Type out_stype = top->getWire("wire_" + outputname);

        // Create LineBuffer component
        LineBuffer *lb = new LineBuffer("LB_" + outputname);
        lb->addInput(inputname, in_stype);
        lb->addOutput(outputname, out_stype);
        size_t num_of_demensions = op->args.size() - 2;
        vector<int> store_extents(num_of_demensions);
        for (size_t i = 2; i < op->args.size(); i++) {
            const IntImm *int_imm = op->args[i].as<IntImm>();
            store_extents[i-2] = int_imm->value;
        }
        lb->setStoreExtents(store_extents);

        // Add to top
        top->addInstance(static_cast<Component*>(lb));

        // Connect clock/reset
        top->addConnect(lb->getInstanceName() + ".clock", "clock");
        top->addConnect(lb->getInstanceName() + ".reset", "reset");

        // Connect LineBuffer input port
        top->addConnect(lb->getInstanceName() + "." + inputname, "wire_" + inputname);     // LB.data_in <= wire

        // Connect LineBuffer Start/Done
        //string done = "LB_" + outputname + "_done";
        //sif->addInPort(done, wire_1bit);
        //lb->addInPort("start_in", wire_1bit);
        //lb->addOutPort("done_out", wire_1bit);
        //top->addConnect(lb->getInstanceName() + ".start_in", sif->getInstanceName() + ".start");  // LB.start_in <= SIF.start
        //top->addConnect(sif->getInstanceName() + "." + done, lb->getInstanceName() + ".done_out");// SIF.done <= LB.done_out

        // Create FIFO following LineBuffer
        FIFO *fifo = new FIFO("FIFO_" + outputname);
        fifo->addInput("data_in", out_stype);
        fifo->addOutput("data_out", out_stype);

        // Add to top
        top->addInstance(static_cast<Component*>(fifo));

        // Connect clock/reset
        top->addConnect(fifo->getInstanceName() + ".clock", "clock");
        top->addConnect(fifo->getInstanceName() + ".reset", "reset");

        // Connect FIFO input port
        top->addConnect(fifo->getInstanceName() + ".data_in", lb->getInstanceName() + "." + outputname);    // FIFO.data_in<=LB.data_out

        // Connect FIFO output port
        //TODO assert, (get->addWires()).contains("wire_" + outputname);
        top->addConnect("wire_" + outputname, fifo->getInstanceName() + ".data_out");  // wire <= FIFO.data_out

        id = "0";
    } else if (op->name == "write_stream") {
        internal_assert(current_fb); // Inside ForBlock
        // normal case
        // IR: write_stream(buffered.stencil_update.stream, buffered.stencil_update)
        const Variable *v0 = op->args[0].as<Variable>();
        const Variable *v1 = op->args[1].as<Variable>();
        string a0 = print_name(v0->name);
        string a1 = print_name(v1->name);
        FIRRTL_Type stream_type = top->getWire("wire_" + a0); // TODO: assert.
        current_fb->addOutput(a0, stream_type);

        // Inside ForBlock, print to ForBlock oss_body directly.
        current_fb->print(a0 + ".value <= " + a1 + "\n");

        // Create FIFO following ForBlock
        FIFO *fifo = new FIFO("FIFO_" + a0);
        fifo->addInput("data_in", stream_type);
        fifo->addOutput("data_out", stream_type);

        // Add to top
        top->addInstance(static_cast<Component*>(fifo));

        // Connect clock/reset
        top->addConnect(fifo->getInstanceName() + ".clock", "clock");
        top->addConnect(fifo->getInstanceName() + ".reset", "reset");

        // Connect FIFO input port from ForBlock output port
        top->addConnect(fifo->getInstanceName() + ".data_in", current_fb->getInstanceName() + "." + a0);

        // Connect FIFO output port
        top->addConnect("wire_" + a0, fifo->getInstanceName() + ".data_out");  // wire <= FIFO.data_out

        if (op->args.size() > 2) {
            // write stream call for the dag output kernel
            // IR: write_stream(output.stencil.stream, output.stencil, loop_var_1, loop_max_1, ...)

            // Create IO component for each input and output
            IO *interface = new IO("IO_" + a0, ComponentType::Output);

            // Add to top
            top->addInstance(static_cast<Component*>(interface));

            interface->addInput(a0, stream_type); // stream
            FIRRTL_Type stype = stream_type;
            stype.type = FIRRTL_Type::StencilContainerType::AxiStream; // stream -> AxiStream
            const Variable *v = op->args[0].as<Variable>();
            string arg_name = print_name(rootName(v->name)); // Use simple name for output.
            interface->addOutput(arg_name, stype); // axi stream
            vector<int> store_extents;
            for (size_t i = 2; i < op->args.size(); i += 2) {
                const IntImm *imm = op->args[i+1].as<IntImm>();
                internal_assert(imm);
                store_extents.push_back(imm->value+1);
            }
            interface->setStoreExtents(store_extents);
            top->addOutput(arg_name, stype);
            //numOutputs++;

            // Connect clock/reset
            top->addConnect(interface->getInstanceName() + ".clock", "clock");
            top->addConnect(interface->getInstanceName() + ".reset", "reset");

            // Connect IO input port
            top->addConnect(interface->getInstanceName() + "." + a0, "wire_" + a0);   // IO.data_in <= wire_fifo_out

            // Connect IO output port
            top->addConnect(arg_name, interface->getInstanceName() + "." + arg_name);

            // Connect IO Start/Done
            string done = "IO_" + a0 + "_done";
            sif->addInPort(done, wire_1bit);
            interface->addInPort("start_in", wire_1bit);
            interface->addOutPort("done_out", wire_1bit);
            top->addConnect(interface->getInstanceName() + ".start_in", sif->getInstanceName() + ".start");    // IO.start_in <= SIF.start
            top->addConnect(sif->getInstanceName() + "." + done, interface->getInstanceName() + ".done_out");  // SIF.done <= IO.done_out
        }
        id = "0";
    } else if (op->name == "read_stream") {
        internal_assert(op->args.size() == 2 || op->args.size() == 3);
        internal_assert(current_fb); // Inside ForBlock, print to ForBlock oss_body.
        string a1 = print_name(print_expr(op->args[1]));

        const Variable *stream_name_var = op->args[0].as<Variable>();
        internal_assert(stream_name_var);
        string stream_name = print_name(stream_name_var->name);
        if (op->args.size() == 3) {
            // stream name is maggled with the consumer name
            const StringImm *consumer_imm = op->args[2].as<StringImm>();
            internal_assert(consumer_imm);
            stream_name += "_to_" + print_name(consumer_imm->value);
        }
        FIRRTL_Type stype = top->getWire("wire_" + stream_name); // get stencil type.
        current_fb->addInput(stream_name, stype);
        top->addConnect(current_fb->getInstanceName() + "." + stream_name, "wire_" + stream_name);

        // Inside ForBlock, print to ForBlock oss_body directly.
        current_fb->print(a1 + " <= " + stream_name + ".value\n");
        id = "0";
    } else if (ends_with(op->name, ".stencil") ||
               ends_with(op->name, ".stencil_update")) {
        ostringstream rhs;
        if (ends_with(op->name, "tap.stencil")) { // tap.stencil is mapped to cmem
            internal_assert(current_fb);
            // Hot Fix for cmem mapped tap stencil.
            // IR: out.stencil_update(0, 0, c)
            // FIRRTL: out_stencil_update_0_0_c : {value : UInt<>, flip addr : UInt<16>[3]}
            // out_stencil_update_0_0_c.addr[0] <= 0
            // out_stencil_update_0_0_c.addr[1] <= 0
            // out_stencil_update_0_0_c.addr[2] <= c
            // node .... <= out_stencil_update_0_0_c.value
            rhs << print_name(op->name) << "_";
            for(int i = op->args.size()-1; i >= 0; i--) {
                const IntImm *idx = op->args[i].as<IntImm>();
                if (idx) { // simplify if possible.
                    rhs << "_" + std::to_string(idx->value);
                } else {
                    rhs << "_" + print_expr(op->args[i]);
                }
            }
            string a = print_name(op->name);
            FIRRTL_Type stype = top->getWire("wire_" + a); // TODO use Scope<>
            stype.type = FIRRTL_Type::StencilContainerType::MemRd; // 
            string wirename = rhs.str() + "_" + current_fb->getInstanceName();
            top->addWire("wire_" + wirename, stype);
            top->addConnect("wire_" + wirename, sif->getInstanceName() + "." + wirename);
            top->addConnect(current_fb->getInstanceName() + "." + rhs.str(), "wire_" + wirename);
            current_fb->addInPort(rhs.str(), stype);
            sif->addOutPort(wirename, stype);
            for(size_t i = 0; i < op->args.size(); i++) {
                current_fb->print(rhs.str()+".addr["+std::to_string(i)+"] <= asUInt("+print_expr(op->args[i])+")\n");
            }
            print_assignment(op->type, rhs.str()+".value");
        } else {
            // IR: out.stencil_update(0, 0, 0)
            // FIRRTL: out_stencil_update[0][0][0]
            rhs << print_name(op->name) << "[";
            for(int i = op->args.size()-1; i >= 0; i--) {
                const IntImm *a  = op->args[i].as<IntImm>();
                if (a) {
                    rhs << std::to_string(a->value);
                } else {
                    rhs << "asUInt(" + print_expr(op->args[i]) + ")";
                }
                if (i != 0)
                    rhs << "][";
            }
            rhs << "]";
            print_assignment(op->type, rhs.str());
        }
    } else if (op->name == "dispatch_stream") {
        // emits the calling arguments in comment
        vector<string> args(op->args.size());
        for(size_t i = 0; i < op->args.size(); i++)
            args[i] = print_name(print_expr(op->args[i]));

        // syntax:
        //   dispatch_stream(stream_name, num_of_dimensions,
        //                   stencil_size_dim_0, stencil_step_dim_0, store_extent_dim_0,
        //                   [stencil_size_dim_1, stencil_step_dim_1, store_extent_dim_1, ...]
        //                   num_of_consumers,
        //                   consumer_0_name, fifo_0_depth,
        //                   consumer_0_offset_dim_0, consumer_0_extent_dim_0,
        //                   [consumer_0_offset_dim_1, consumer_0_extent_dim_1, ...]
        //                   [consumer_1_name, ...])

        // recover the structed data from op->args
        internal_assert(op->args.size() >= 2);
        const Variable *stream_name_var = op->args[0].as<Variable>();
        internal_assert(stream_name_var);
        string stream_name = print_name(stream_name_var->name);
        size_t num_of_demensions = *as_const_int(op->args[1]);
        vector<int> stencil_sizes(num_of_demensions);
        vector<int> stencil_steps(num_of_demensions);
        vector<int> store_extents(num_of_demensions);

        internal_assert(op->args.size() >= num_of_demensions*3 + 2);
        for (size_t i = 0; i < num_of_demensions; i++) {
            stencil_sizes[i] = *as_const_int(op->args[i*3 + 2]);
            stencil_steps[i] = *as_const_int(op->args[i*3 + 3]);
            store_extents[i] = *as_const_int(op->args[i*3 + 4]);
        }

        internal_assert(op->args.size() >= num_of_demensions*3 + 3);
        size_t num_of_consumers = *as_const_int(op->args[num_of_demensions*3 + 2]);
        vector<string> consumer_names(num_of_consumers);
        vector<int> consumer_fifo_depth(num_of_consumers);
        vector<vector<int> > consumer_offsets(num_of_consumers);
        vector<vector<int> > consumer_extents(num_of_consumers);

        internal_assert(op->args.size() >= num_of_demensions*3 + 3 + num_of_consumers*(2 + 2*num_of_demensions));
        for (size_t i = 0; i < num_of_consumers; i++) {
            const StringImm *string_imm = op->args[num_of_demensions*3 + 3 + (2 + 2*num_of_demensions)*i].as<StringImm>();
            internal_assert(string_imm);
            consumer_names[i] = string_imm->value;
            const IntImm *int_imm = op->args[num_of_demensions*3 + 4 + (2 + 2*num_of_demensions)*i].as<IntImm>();
            internal_assert(int_imm);
            consumer_fifo_depth[i] = int_imm->value; // TODO: We will calculate this automatically.
            vector<int> offsets(num_of_demensions);
            vector<int > extents(num_of_demensions);
            for (size_t j = 0; j < num_of_demensions; j++) {
                offsets[j] = *as_const_int(op->args[num_of_demensions*3 + 5 + (2 + 2*num_of_demensions)*i + 2*j]);
                extents[j] = *as_const_int(op->args[num_of_demensions*3 + 6 + (2 + 2*num_of_demensions)*i + 2*j]);
            }
            consumer_offsets[i] = offsets;
            consumer_extents[i] = extents;
        }

        // emits declarations of streams for each consumer
        //internal_assert(stencils.contains(stream_name));
        //Stencil_Type stream_type = stencils.get(stream_name);
        FIRRTL_Type stream_type = top->getWire("wire_" + stream_name);

        // Optimization. if there is only one consumer and its fifo depth is zero
        // , use wire connection for the consumer stream
        if (num_of_consumers == 1 && consumer_fifo_depth[0] == 0) {
            string consumer_stream_name = stream_name + "_to_" + print_name(consumer_names[0]);
            //stream << print_stencil_type(stream_type) << " &"
            //       << print_name(consumer_stream_name) << " = "
            //       << print_name(stream_name) << ";\n";
            top->addWire("wire_" + consumer_stream_name, stream_type);
            top->addConnect("wire_" + consumer_stream_name, "wire_" + stream_name);

            id = "0"; // skip evaluation
            return;
        }

        for (size_t i = 0; i < num_of_consumers; i++) {
            consumer_fifo_depth[i] = std::max(consumer_fifo_depth[i], 1);// set minimum. TODO: We will calculate this automatically.
        }
        // Create Dispatch component
        Dispatch *dp = new Dispatch("DP_" + stream_name);
        dp->addInput(stream_name, stream_type);
        dp->setStencilSizes(stencil_sizes); // TODO Can't it be gotten from stencil_type?
        dp->setStencilSteps(stencil_steps);
        dp->setStoreExtents(store_extents);
        dp->setConsumerFifoDepths(consumer_fifo_depth);
        dp->setConsumerOffsets(consumer_offsets);
        dp->setConsumerExtents(consumer_extents);

        // Add to top
        top->addInstance(static_cast<Component*>(dp));

        // Connect clock/reset
        top->addConnect(dp->getInstanceName() + ".clock", "clock");
        top->addConnect(dp->getInstanceName() + ".reset", "reset");

        // Connect Dispatch input port
        top->addConnect(dp->getInstanceName() + "." + stream_name, "wire_" + stream_name);

        // Connect Dispatch Start/Done
        string done = "DP_" + stream_name + "_done";
        sif->addInPort(done, wire_1bit);
        dp->addInPort("start_in", wire_1bit);
        dp->addOutPort("done_out", wire_1bit);
        top->addConnect(dp->getInstanceName() + ".start_in", sif->getInstanceName() + ".start");   // DP.start_in <= SIF.start
        top->addConnect(sif->getInstanceName() + "." + done, dp->getInstanceName() + ".done_out"); // SIF.done <= DP.done_out

        for (size_t i = 0; i < num_of_consumers; i++) {
            string consumer_stream_name = stream_name + "_to_" + print_name(consumer_names[i]);
            dp->addOutput(consumer_stream_name, stream_type);

            // Create FIFO following Dispatch for each output.
            FIFO *fifo = new FIFO("FIFO_" + consumer_stream_name);
            fifo->addInput("data_in", stream_type);
            fifo->addOutput("data_out", stream_type);
            //fifo->setDepth("$$"); // Mark to be back-annotated later TODO
            fifo->setDepth(std::to_string(consumer_fifo_depth[i])); // FIXME later

            // Add to top
            top->addInstance(static_cast<Component*>(fifo));

            // Connect clock/reset
            top->addConnect(fifo->getInstanceName() + ".clock", "clock");
            top->addConnect(fifo->getInstanceName() + ".reset", "reset");

            // Connect FIFO input port
            top->addConnect(fifo->getInstanceName() + ".data_in",  dp->getInstanceName() + "." + consumer_stream_name);

            // Connect FIFO output port
            top->addWire("wire_" + consumer_stream_name, stream_type);
            top->addConnect("wire_" + consumer_stream_name, fifo->getInstanceName() + ".data_out");
        }

        id = "0";
    }
}

void CodeGen_FIRRTL_Target::visit(const Load *op)
{
    ostringstream rhs;
    Type t = op->type;
    string name = print_name(op->name);

    string id_index = print_expr(op->index);
    rhs << name;
    rhs << "[asUInt(" << id_index << ")]";

    print_assignment(t, rhs.str());
}

void CodeGen_FIRRTL_Target::visit(const Store *op)
{
    debug(3) << "CodeGen_FIRRTL_Target::visit(Store) " << op->name << "\n";

    string id_value = print_expr(op->value);
    string id_index = print_expr(op->index);
    string name = print_name(op->name);

    internal_assert(current_fb); // for now Allocate/Store/Load is supported only inside of for-loop body.
    current_fb->print(name + "[asUInt(" + id_index + ")] <= " + id_value + "\n");

    cache.clear();
}

void CodeGen_FIRRTL_Target::visit(const Let *op)
{
    string id_value = print_expr(op->value);
    Expr new_var = Variable::make(op->value.type(), id_value);
    Expr body = substitute(op->name, new_var, op->body);
    print_expr(body);
}

void CodeGen_FIRRTL_Target::visit(const Select *op)
{
    ostringstream rhs;
    string type;
    string true_val = print_expr(op->true_value);
    string false_val = print_expr(op->false_value);
    string cond = print_expr(op->condition);

    if ((op->type).is_uint()) {
        type = "asUInt(";
    } else {
        type = "asSInt(";
    }
    rhs << type << "mux(" << cond
        << ", " << type << true_val << ")"
        << ", " << type << false_val << ")))";
    print_assignment(op->type, rhs.str());
}

void CodeGen_FIRRTL_Target::visit(const LetStmt *op)
{
    stream << "; LetStmt ??\n"; // FIXME
    string id_value = print_expr(op->value);
    Expr new_var = Variable::make(op->value.type(), id_value);
    Stmt body = substitute(op->name, new_var, op->body);
    body.accept(this);
}

void CodeGen_FIRRTL_Target::visit(const AssertStmt *op)
{
    internal_error << "AsserStmt is not supported.\n";
}

void CodeGen_FIRRTL_Target::visit(const ProducerConsumer *op)
{
    if (ends_with(op->name, ".stream")) {
        producename = op->name; // Used as a name containing ForBlock name.
    }
    print_stmt(op->body);
}

void CodeGen_FIRRTL_Target::visit(const For *op)
{
    internal_assert(op->for_type == ForType::Serial)
        << "Can only emit serial for loops to FIRRTL\n";

    FIRRTL_Type wire_1bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(1),Region(),0,{}};
    FIRRTL_Type wire_8bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(8),Region(),0,{}};
    FIRRTL_Type wire_16bit = {FIRRTL_Type::StencilContainerType::Scalar,UInt(16),Region(),0,{}};

    string var_name = print_name(op->name);
    int id_min = ((op->min).as<IntImm>())->value;
    int id_extent = ((op->extent).as<IntImm>())->value;

    if (for_scanvar_list.empty()) { // First for of for-loop group. Only one ForBlock per For-loop group.

        // Create ForBlock component
        ForBlock *fb = new ForBlock("FB_" + print_name(producename));
        current_fb = fb;

        // Add to top
        top->addInstance(static_cast<Component*>(fb));

        // Connect clock/reset
        top->addConnect(fb->getInstanceName() + ".clock", "clock");
        top->addConnect(fb->getInstanceName() + ".reset", "reset");

        // Connect Start/Done
        string done = "FB_" + print_name(producename) + "_done";
        sif->addInPort(done, wire_1bit);
        fb->addInPort("start_in", wire_1bit);
        fb->addOutPort("done_out", wire_1bit);
        fb->addVar(var_name); // Outermost for loop var is never stencil var.
        fb->addMin(id_min);
        fb->addMax(id_extent-1);
        for_scanvar_list.push_back(var_name);
        top->addConnect(fb->getInstanceName() + ".start_in", sif->getInstanceName() + ".start");
        top->addConnect(sif->getInstanceName() + "." + done, fb->getInstanceName() + ".done_out");

        // Add parameter ports and connect them
        FIRRTL_For_Closure c(op->body);
        // Note: Outermost op->name can be added to Closure because only op->body is processed.
        // op->name will be excluded from Closure result by checking for_scanvar_list[0].
        vector<string> args = c.arguments(); // extract used variables.
        for(auto &s: args) { // Create ports and connect for variables.
            string a = print_name(s);
            if (for_scanvar_list[0] != a) { // ignore scan var, TODO Do we need this? Better way?
                if (!ends_with(s, "tap.stencil")) { // tap_stencil will be added later.
                    FIRRTL_Type stype = top->getWire("wire_" + a);
                    fb->addInPort(a, stype);
                    top->addConnect(fb->getInstanceName() + "." + a, "wire_" + a);
                }
            }
        }

    } else {
        internal_assert(current_fb);
        // If ForBlock is already created, just add loop variable ports and loop bound.
        if (!contain_realize(op->body)) { // this is variable iterates over stencil. TODO: better way?
            current_fb->addStencilVar(var_name);
            current_fb->addStencilMin(id_min);
            current_fb->addStencilMax(id_extent-1);
        } else {
            current_fb->addVar(var_name);
            current_fb->addMin(id_min);
            current_fb->addMax(id_extent-1);
        }
        for_scanvar_list.push_back(var_name);
    }

    if (!contain_for_loop(op->body)) { // inner most loop
        cache.clear();
    }

    print(op->body);

    for_scanvar_list.pop_back();

    if (for_scanvar_list.empty()) {
        cache.clear();
        current_fb = nullptr;
    }
}

void CodeGen_FIRRTL_Target::visit(const Provide *op)
{
    if (ends_with(op->name, ".stencil") ||
        ends_with(op->name, ".stencil_update")) {
        ostringstream oss;
        // IR: buffered.stencil_update(1, 2, 3) =
        // FIRRTL: buffered_stencil_update[1][2][3] =
        //vector<string> args_indices(op->args.size());
        vector<string> args_indices(op->args.size());
        for(size_t i = 0; i < op->args.size(); i++) {
            const IntImm *e = op->args[i].as<IntImm>();
            if (e) {
                args_indices[i] = std::to_string(e->value);
            } else {
                const Variable *v = op->args[i].as<Variable>();
                internal_assert(v);
                args_indices[i] = "asUInt(" + print_name(v->name) + ")";
            }
        }

        internal_assert(op->values.size() == 1);
        string id_value = print_expr(op->values[0]);

        oss << print_name(op->name) << "[";

        for(int i = op->args.size()-1; i >= 0; i--) { // reverse order in FIRRTL
            oss << args_indices[i];
            if (i != 0)
                oss << "][";
        }
        oss << "]";
        if (current_fb!=nullptr) {
          // Inside ForBlock, print to ForBlock oss_body directly.
          current_fb->print(oss.str() + " <= " + id_value + "\n");
        } else { // TODO Do we need this?
            internal_assert(false) << "Provide at outside of ForBlock\n";
            top->addConnect(oss.str(), id_value);
        }

        cache.clear();
    } else {
        IRPrinter::visit(op);
    }
}

// Copied from CodeGen_HLS_Target.cpp and renamed.
class FirrtlRenameAllocation : public IRMutator {
    const string &orig_name;
    const string &new_name;

    using IRMutator::visit;

    void visit(const Load *op) {
        if (op->name == orig_name ) {
            Expr index = mutate(op->index);
            expr = Load::make(op->type, new_name, index, op->image, op->param, op->predicate);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Store *op) {
        if (op->name == orig_name ) {
            Expr value = mutate(op->value);
            Expr index = mutate(op->index);
            stmt = Store::make(new_name, value, index, op->param, op->predicate);
        } else {
            IRMutator::visit(op);
        }
    }

    void visit(const Free *op) {
        if (op->name == orig_name) {
            stmt = Free::make(new_name);
        } else {
            IRMutator::visit(op);
        }
    }

public:
    FirrtlRenameAllocation(const string &o, const string &n)
        : orig_name(o), new_name(n) {}
};

// most code is copied from CodeGen_HLS_Target::visit(const Allocate *)
void CodeGen_FIRRTL_Target::visit(const Allocate *op)
{
    //stream << "reg " << op->name << ": ____TODO____\n";
    //print(op->body);
    internal_assert(!op->new_expr.defined());
    internal_assert(!is_zero(op->condition));
    int32_t constant_size;
    constant_size = op->constant_allocation_size();
    if (constant_size > 0) {

    } else {
        internal_error << "Size for allocation " << op->name
                       << " is not a constant.\n";
    }

    debug(3) << "CodeGen_FIRRTL_Target::visit(Allocate) " << op->name << " size=" << constant_size << " type=" << print_type(op->type) << "\n";

    // rename allocation to avoid name conflict due to unrolling
    string alloc_name = op->name + unique_name('a');
    Stmt new_body = FirrtlRenameAllocation(op->name, alloc_name).mutate(op->body);

    internal_assert(current_fb); // for now Allocate is supported only inside of for-loop body.
    Region bounds;
    bounds.push_back(Range(0, constant_size));
    FIRRTL_Type stencil_type({FIRRTL_Type::StencilContainerType::Stencil, op->type, bounds, 1, {}});
    current_fb->addReg(print_name(alloc_name), stencil_type);

    new_body.accept(this);

}

void CodeGen_FIRRTL_Target::visit(const Free *op)
{
    // Hardware register cannot be freed.
}

void CodeGen_FIRRTL_Target::visit(const Realize *op)
{
    if (ends_with(op->name, ".stream")) {
        internal_assert(op->types.size() == 1);
        std::vector<int> store_extents;
        for(size_t i = 0; i < op->bounds.size(); i++) store_extents.push_back(1); // default
        FIRRTL_Type stream_type({FIRRTL_Type::StencilContainerType::Stream,
                    op->types[0], op->bounds, 1, store_extents});
        top->addWire("wire_" + print_name(op->name), stream_type);

        // traverse down
        op->body.accept(this);

    } else if (ends_with(op->name, ".stencil") ||
               ends_with(op->name, ".stencil_update")) {
        //internal_assert(op->types.size() == 1); // TODO ??
        internal_assert(current_fb);
        std::vector<int> store_extents;
        for(size_t i = 0; i < op->bounds.size(); i++) store_extents.push_back(1); // default
        FIRRTL_Type stream_type({FIRRTL_Type::StencilContainerType::Stencil,
                    op->types[0], op->bounds, 1, store_extents});
        if (starts_with(producename, op->name)) { // Output stencil, map to register.
            current_fb->addReg(print_name(op->name), stream_type);
        } else { // Input stencil can be a wire. The value will stay there until it is popped from previous FIFO.
            current_fb->addWire(print_name(op->name), stream_type);
        }

        op->body.accept(this);

    } else {
        visit(op);
    }
}

void CodeGen_FIRRTL_Target::visit(const IfThenElse *op) {
    if (current_fb!=nullptr) {
        // Inside ForBlock, print to ForBlock oss_body then print to stream later.
        // for(y...)
        //   for(x...)
        //     for(c...)
        //       if(c==0) read_stream() // no need. Wire output from previous FIFO is enough.
        //       out(0,0,c) = ...       // @ clock 0
        //       if(c==2) write_stream()// @ clock 1, c is not the same c, it's delayed c.
        if (contain_read_stream(op->then_case)) {
            print(op->then_case);
        } else if (contain_write_stream(op->then_case)) {
            current_fb->print("when " + print_expr(op->condition) + " :\n");
            current_fb->open_scope();
            print(op->then_case);
            current_fb->close_scope("");
        } else {
            internal_error << "General IfThenElse is not supported.\n"; // TODO
        }
    } else {
        internal_error << "General IfThenElse is not supported.\n"; // TODO
    }
    id = "0";
}

void CodeGen_FIRRTL_Target::visit(const Evaluate *op)
{
    if (is_const(op->value)) return;
    string id = print_expr(op->value);
}

}
}
