// Copyright information found in source file:
// Copyright 2022 ETH Zurich and University of Bologna.

// Licensing information found in source file:
// Licensed under Solderpad Hardware License, Version 0.51, see LICENSE for details.
// SPDX-License-Identifier: SHL-0.51

#include <vector>
#include <vp/itf/io.hpp>
#include <vp/vp.hpp>

#include "archi/carfield_pcrs.h"


struct IsolateRegs {
    uint32_t periph;
    uint32_t safety_island;
    uint32_t security_island;
    uint32_t pulp_cluster;
    uint32_t spatz_cluster;
    uint32_t l2;
};

struct ClockRegs {
    uint32_t periph;
    uint32_t safety_island;
    uint32_t security_island;
    uint32_t pulp_cluster;
    uint32_t spatz_cluster;
    uint32_t l2;
};

struct PulpClusterRegs {
    uint32_t fetch_enable;
    uint32_t boot_enable;
    uint32_t busy;
    uint32_t eoc;
};

class PlatformControlRegs : public vp::Component
{
  public:
    PlatformControlRegs(vp::ComponentConf &config);
    void reset(bool active);
    static vp::IoReqStatus req(vp::Block *__this, vp::IoReq *req);

  private:
    // Pulp cluster registers handlers TODO: stubbed
    vp::IoReqStatus handle_fetch_enable(bool is_write, uint32_t *data);
    vp::IoReqStatus handle_boot_enable(bool is_write, uint32_t *data);
    vp::IoReqStatus handle_busy(bool is_write, uint32_t *data);
    vp::IoReqStatus handle_eoc(bool is_write, uint32_t *data);
    // Isolate and clock handlers for each domain
    vp::IoReqStatus handle_isolate(uint64_t offset, bool is_write, uint32_t *data);
    vp::IoReqStatus handle_clk_enable(uint64_t offset, bool is_write, uint32_t *data);


    // PULPD registers
    PulpClusterRegs pulp_cluster_regs;
    IsolateRegs isolate_regs;
    ClockRegs clock_regs;

    vp::Trace trace;
    vp::IoSlave in;
};

PlatformControlRegs::PlatformControlRegs(vp::ComponentConf &config) : vp::Component(config)
{
    this->traces.new_trace("trace", &trace, vp::DEBUG);
    this->in.set_req_meth(&PlatformControlRegs::req);
    this->new_slave_port("input", &this->in);
}

void PlatformControlRegs::reset(bool active)
{
    if (active) {
        this->pulp_cluster_regs.fetch_enable = 0;
        this->pulp_cluster_regs.boot_enable  = 0;
        this->pulp_cluster_regs.busy         = 0;
        this->pulp_cluster_regs.eoc          = 0;

    this->isolate_regs.periph = 0;
    this->isolate_regs.safety_island = 0;
    this->isolate_regs.security_island = 0;
    this->isolate_regs.pulp_cluster = 0;
    this->isolate_regs.spatz_cluster = 0;
    this->isolate_regs.l2 = 0;

        this->clock_regs.periph = 0;
        this->clock_regs.safety_island = 0;
        this->clock_regs.security_island = 0;
        this->clock_regs.pulp_cluster = 0;
        this->clock_regs.spatz_cluster = 0;
        this->clock_regs.l2 = 0;
    }
}

extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
    return new PlatformControlRegs(config);
}

vp::IoReqStatus PlatformControlRegs::req(vp::Block *__this, vp::IoReq *req)
{
    PlatformControlRegs *_this = (PlatformControlRegs *)__this;
    uint64_t offset            = req->get_addr();
    bool is_write              = req->get_is_write();
    uint64_t size              = req->get_size();
    uint8_t *data              = req->get_data();

    _this->trace.msg(vp::Trace::LEVEL_DEBUG, "Received IO req (offset: 0x%llx, size: 0x%llx, is_write: %d)\n", offset,
                     size, is_write);

    if (size != 4) {
        _this->trace.warning("Only 32 bits accesses are allowed\n");
        return vp::IO_REQ_INVALID;
    }

    // Handle fetch enable registers
    if (offset == CARFIELD_PULP_CLUSTER_FETCH_ENABLE_REG_OFFSET) {
        return _this->handle_fetch_enable(is_write, (uint32_t *)data);
    }
    // Handle boot enable registers
    else if (offset == CARFIELD_PULP_CLUSTER_BOOT_ENABLE_REG_OFFSET) {
        return _this->handle_boot_enable(is_write, (uint32_t *)data);
    }
    // Handle busy registers
    else if (offset == CARFIELD_PULP_CLUSTER_BUSY_REG_OFFSET) {
        return _this->handle_busy(is_write, (uint32_t *)data);
    }
    // Handle end of computation registers
    else if (offset == CARFIELD_PULP_CLUSTER_EOC_REG_OFFSET) {
        return _this->handle_eoc(is_write, (uint32_t *)data);
    }
    // Handle isolate for each domain

    // CARFIELD_PERIPH_ISOLATE_REG_OFFSET -> CARFIELD_L2_ISOLATE_REG_OFFSET
    // CARFIELD_PERIPH_ISOLATE_STATUS_REG_OFFSET -> CARFIELD_L2_ISOLATE_STATUS_REG_OFFSET
    // TODO: data and status are using the same regs, need to differentiate to simulate delay in settings
    else if (offset >= CARFIELD_PERIPH_ISOLATE_REG_OFFSET && offset <= CARFIELD_L2_ISOLATE_REG_OFFSET ||
             offset >= CARFIELD_PERIPH_ISOLATE_STATUS_REG_OFFSET && offset <= CARFIELD_L2_ISOLATE_STATUS_REG_OFFSET) {
        return _this->handle_isolate(offset, is_write, (uint32_t *)data);
    }
    // Handle enable clocks for each domain
    else if (offset >= CARFIELD_PERIPH_CLK_EN_REG_OFFSET && offset <= CARFIELD_L2_CLK_EN_REG_OFFSET) {
        return _this->handle_clk_enable(offset, is_write, (uint32_t *)data);
    }

    _this->trace.force_warning("Invalid access\n");
    return vp::IO_REQ_INVALID;
}

// Handler functions

vp::IoReqStatus PlatformControlRegs::handle_fetch_enable(bool is_write, uint32_t *data)
{
    trace.msg("Accessing PULP_CLUSTER_FETCH_ENABLE (is_write: %d, value: 0x%x)\n", is_write, *data);
    if (is_write) {
        pulp_cluster_regs.fetch_enable = *data;
    } else {
        *data = pulp_cluster_regs.fetch_enable;
    }
    return vp::IO_REQ_OK;
}

vp::IoReqStatus PlatformControlRegs::handle_boot_enable(bool is_write, uint32_t *data)
{
    trace.msg("Accessing PULP_CLUSTER_BOOT_ENABLE (is_write: %d, value: 0x%x)\n", is_write, *data);
    if (is_write) {
        pulp_cluster_regs.boot_enable = *data;
    } else {
        *data = pulp_cluster_regs.boot_enable;
    }
    return vp::IO_REQ_OK;
}

vp::IoReqStatus PlatformControlRegs::handle_busy(bool is_write, uint32_t *data)
{
    trace.msg("Accessing PULP_CLUSTER_BUSY (is_write: %d, value: 0x%x)\n", is_write, *data);
    if (is_write) {
        pulp_cluster_regs.busy = *data;
    } else {
        *data = pulp_cluster_regs.busy;
    }
    return vp::IO_REQ_OK;
}

vp::IoReqStatus PlatformControlRegs::handle_eoc(bool is_write, uint32_t *data)
{
    trace.msg("Accessing PULP_CLUSTER_EOC (is_write: %d, value: 0x%x)\n", is_write, *data);
    if (is_write) {
        pulp_cluster_regs.eoc = *data;
    } else {
        *data = pulp_cluster_regs.eoc;
    }
    return vp::IO_REQ_OK;
}


vp::IoReqStatus PlatformControlRegs::handle_isolate(uint64_t offset, bool is_write, uint32_t *data)
{
    if (offset == CARFIELD_PERIPH_ISOLATE_REG_OFFSET || offset == CARFIELD_PERIPH_ISOLATE_STATUS_REG_OFFSET) {
        if (is_write) isolate_regs.periph = *data;
        else *data = isolate_regs.periph;
    } else if (offset == CARFIELD_SAFETY_ISLAND_ISOLATE_REG_OFFSET || offset == CARFIELD_SAFETY_ISLAND_ISOLATE_STATUS_REG_OFFSET) {
        if (is_write) isolate_regs.safety_island = *data;
        else *data = isolate_regs.safety_island;
    } else if (offset == CARFIELD_SECURITY_ISLAND_ISOLATE_REG_OFFSET || offset == CARFIELD_SECURITY_ISLAND_ISOLATE_STATUS_REG_OFFSET) {
        if (is_write) isolate_regs.security_island = *data;
        else *data = isolate_regs.security_island;
    } else if (offset == CARFIELD_PULP_CLUSTER_ISOLATE_REG_OFFSET || offset == CARFIELD_PULP_CLUSTER_ISOLATE_STATUS_REG_OFFSET) {
        if (is_write) isolate_regs.pulp_cluster = *data;
        else *data = isolate_regs.pulp_cluster;
    } else if (offset == CARFIELD_SPATZ_CLUSTER_ISOLATE_REG_OFFSET || offset == CARFIELD_SPATZ_CLUSTER_ISOLATE_STATUS_REG_OFFSET) {
        if (is_write) isolate_regs.spatz_cluster = *data;
        else *data = isolate_regs.spatz_cluster;
    } else if (offset == CARFIELD_L2_ISOLATE_REG_OFFSET || offset == CARFIELD_L2_ISOLATE_STATUS_REG_OFFSET) {
        if (is_write) isolate_regs.l2 = *data;
        else *data = isolate_regs.l2;
    }
    return vp::IO_REQ_OK;
}

vp::IoReqStatus PlatformControlRegs::handle_clk_enable(uint64_t offset, bool is_write, uint32_t *data)
{
    if (offset == CARFIELD_PERIPH_CLK_EN_REG_OFFSET) {
        if (is_write) clock_regs.periph = *data;
        else *data = clock_regs.periph;
    } else if (offset == CARFIELD_SAFETY_ISLAND_CLK_EN_REG_OFFSET) {
        if (is_write) clock_regs.safety_island = *data;
        else *data = clock_regs.safety_island;
    } else if (offset == CARFIELD_SECURITY_ISLAND_CLK_EN_REG_OFFSET) {
        if (is_write) clock_regs.security_island = *data;
        else *data = clock_regs.security_island;
    } else if (offset == CARFIELD_PULP_CLUSTER_CLK_EN_REG_OFFSET) {
        if (is_write) clock_regs.pulp_cluster = *data;
        else *data = clock_regs.pulp_cluster;
    } else if (offset == CARFIELD_SPATZ_CLUSTER_CLK_EN_REG_OFFSET) {
        if (is_write) clock_regs.spatz_cluster = *data;
        else *data = clock_regs.spatz_cluster;
    } else if (offset == CARFIELD_L2_CLK_EN_REG_OFFSET) {
        if (is_write) clock_regs.l2 = *data;
        else *data = clock_regs.l2;
    }
    return vp::IO_REQ_OK;
}
