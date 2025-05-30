/*
 * Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and
 *                    University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */

#include <vp/vp.hpp>
#include <vp/itf/io.hpp>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "archi/utils.h"
#include "archi/udma_v3.h"
#include "udma_v3_impl.hpp"

#ifdef HAS_MRAM
#include "mram/udma_mram_v1.hpp"
#endif

#if defined(I2S_VERSION)
#if I2S_VERSION == 2
#include "i2s/udma_i2s_v2.hpp"
#endif
#if I2S_VERSION == 3
#include "i2s/udma_i2s_v3.hpp"
#endif
#endif



void Udma_rx_channel::push_data(uint8_t *data, int size)
{
  if (current_cmd == NULL)
  {
    //top->warning.warning("Received data while there is no ready command\n");
    return;
  }

  if (size + this->pending_byte_index > 4)
  {
    trace.force_warning("Trying to push more than 4 bytes from peripheral to udma core\n");
    return;
  }

  memcpy(&(((uint8_t *)&this->pending_word)[this->pending_byte_index]), data, size);

  this->pending_byte_index += size;

  if (this->pending_byte_index >= 4 || this->pending_byte_index >= current_cmd->remaining_size)
  {
    this->pending_byte_index = 0;
    vp::IoReq *req = this->top->l2_itf.req_new(0, new uint8_t[4], 4, true);
    *(uint32_t *)req->get_data() = this->pending_word;
    bool end = current_cmd->prepare_req(req);
    trace.msg("Writing 4 bytes to memory (value: 0x%x, addr: 0x%x)\n", this->pending_word, req->get_addr());
    this->top->push_l2_write_req(req);
    if (end)
    {
      handle_transfer_end();
    }
  }
}

void Udma_rx_channel::reset(bool active)
{
  Udma_channel::reset(active);

  if (active)
  {
    pending_byte_index = 0;
  }
}



void Udma_channel::handle_transfer_end()
{
  trace.msg("Current transfer is finished\n");
  free_reqs->push(current_cmd);
  current_cmd = NULL;
  top->trigger_event(id);
  this->check_state();
}

void Udma_channel::handle_ready_reqs()
{
  if (!ready_reqs->is_empty())
  {
    vp::IoReq *req = ready_reqs->pop();
    handle_ready_req_end(req);
  }
}

void Udma_channel::handle_ready_req(vp::IoReq *req)
{
  ready_reqs->push(req);
  handle_ready_reqs();
}

bool Udma_channel::handle_ready_req_end(vp::IoReq *req)
{
  bool end = false;
  if (current_cmd && current_cmd->received_size >= current_cmd->size)
  {
    handle_transfer_end();
    end = true;
  }
  top->free_read_req(req);
  return end;
}

void Udma_channel::push_ready_req(vp::IoReq *req)
{
  current_cmd->received_size += req->get_size();

  trace.msg("Received data from L2 (cmd: %p, data_size: 0x%x, transfer_size: 0x%x, received_size: 0x%x, value: 0x%x)\n",
    current_cmd, req->get_size(), current_cmd->size, current_cmd->received_size, *(uint32_t *)req->get_data());

  handle_ready_req(req);
}

vp::IoReqStatus Udma_channel::saddr_req(vp::IoReq *req)
{
  uint32_t *data = (uint32_t *)req->get_data();
  if (req->get_is_write())
  {
    trace.msg("Setting saddr register (value: 0x%x)\n", *data);
    saddr = *data;
  }
  else
  {
    *data = saddr;
  }
  return vp::IO_REQ_OK;
}



vp::IoReqStatus Udma_channel::size_req(vp::IoReq *req)
{
  uint32_t *data = (uint32_t *)req->get_data();
  if (req->get_is_write())
  {
    trace.msg("Setting size register (value: 0x%x)\n", *data);
    size = *(int32_t *)data;
  }
  else
  {
    *data = size;
  }
  return vp::IO_REQ_OK;
}


void Udma_channel::event_handler()
{
  if (!pending_reqs->is_empty() && current_cmd == NULL)
  {
    current_cmd = pending_reqs->pop();
    trace.msg("New ready transfer (cmd: %p)\n", current_cmd);
    top->enqueue_ready(this);
  }
}

void Udma_channel::check_state()
{
  if (!pending_reqs->is_empty() && current_cmd == NULL)
  {
    top->event_enqueue_ext(event, 1);
  }

  if (free_reqs->is_full())
  {
    this->state_event.event_highz();
  }
  else
  {
    uint8_t one = 1;
    this->state_event.event(&one);
  }
}


void Udma_channel::build_reqs_and_enqueue(Udma_transfer *req)
{
  saddr = req->addr;
  size = req->size;
  transfer_size = req->transfer_size;
  continuous_mode = req->continuous_mode;

  enqueue_transfer();
}


void Udma_channel::enqueue_transfer()
{
  if (free_reqs->is_empty())
  {
    vp_warning_always(&this->trace, "Trying to enqueue transfer while already 2 are pending\n");
    return;
  }

  Udma_transfer *req = free_reqs->pop();

  trace.msg("Enqueueing new transfer (req: %p, addr: 0x%x, size: 0x%x, transfer_size: %s, continuous: %d)\n",
    req, saddr, size, transfer_size == 0 ? "8bits" : transfer_size == 1 ? "16bits" : "32bits",
    continuous_mode);

  req->addr = saddr;
  req->current_addr = saddr;
  req->size = size;
  req->remaining_size = size;
  req->transfer_size = transfer_size;
  req->received_size = 0;
  req->continuous_mode = continuous_mode;
  req->channel = this;
  pending_reqs->push(req);

  check_state();
}



vp::IoReqStatus Udma_channel::cfg_req(vp::IoReq *req)
{
  uint32_t *data = (uint32_t *)req->get_data();
  if (req->get_is_write())
  {
    continuous_mode = ARCHI_REG_FIELD_GET(*data, UDMA_CHANNEL_CFG_CONT_BIT, 1);
    transfer_size = ARCHI_REG_FIELD_GET(*data, UDMA_CHANNEL_CFG_SIZE_BIT, 1);
    bool channel_enabled = ARCHI_REG_FIELD_GET(*data, UDMA_CHANNEL_CFG_EN_BIT, 1);
    bool channel_clear = ARCHI_REG_FIELD_GET(*data, UDMA_CHANNEL_CFG_CLEAR_BIT, 1);
    trace.msg("Setting cfg register (continuous: %d, size: %s, enable: %d, clear: %d)\n",
      continuous_mode, transfer_size == 0 ? "8bits" : transfer_size == 1 ? "16bits" : "32bits",
      channel_enabled, channel_clear);

    if (channel_clear)
    {
      trace.msg("UNIMPLEMENTED AT %s %d\n", __FILE__, __LINE__);
      return vp::IO_REQ_INVALID;
    }

    if (channel_enabled)
    {
      enqueue_transfer();
    }
  }
  else
  {
    *data = (continuous_mode << UDMA_CHANNEL_CFG_CONT_BIT) |
      (transfer_size << UDMA_CHANNEL_CFG_SIZE_BIT) |
      (!free_reqs->is_full() << UDMA_CHANNEL_CFG_EN_BIT) |
      (free_reqs->is_empty() << UDMA_CHANNEL_CFG_SHADOW_BIT);
  }
  return vp::IO_REQ_OK;
}



vp::IoReqStatus Udma_channel::req(vp::IoReq *req, uint64_t offset)
{
  if (offset == UDMA_CHANNEL_SADDR_OFFSET)
  {
    return saddr_req(req);
  }
  else if (offset == UDMA_CHANNEL_SIZE_OFFSET)
  {
    return size_req(req);
  }
  else if (offset == UDMA_CHANNEL_CFG_OFFSET)
  {
    return cfg_req(req);
  }
  return vp::IO_REQ_INVALID;
}



Udma_channel::Udma_channel(udma *top, int id, string name) : top(top), id(id), name(name)
{
  top->traces.new_trace(name + "/trace", &trace, vp::DEBUG);

  // Each channel can handle 2 transfers at the same time
  free_reqs = new Udma_queue<Udma_transfer>(2);
  pending_reqs = new Udma_queue<Udma_transfer>(2);
  ready_reqs = new Udma_queue<vp::IoReq>(-1);
  free_reqs->push( new Udma_transfer());
  free_reqs->push( new Udma_transfer());

  event = top->event_new((vp::Block *)this, udma::channel_handler);

  top->traces.new_trace_event(name + "/state", &this->state_event, 8);
}



bool Udma_channel::prepare_req(vp::IoReq *req)
{
  return current_cmd->prepare_req(req);
}



void Udma_channel::reset(bool active)
{
  if (active)
  {
    current_cmd = NULL;
    continuous_mode = 0;
    transfer_size = 0;
    this->state_event.event_highz();
  }
}


Udma_periph::Udma_periph(udma *top, int id) : top(top), id(id)
{
}



void Udma_periph::clock_gate(bool new_is_on)
{
  if (is_on != new_is_on)
  {
    if (new_is_on)
      top->trace.msg("Activating periph (periph: %d)\n", id);
    else
      top->trace.msg("Dectivating periph (periph: %d)\n", id);
  }
  is_on = new_is_on;
}



void Udma_periph::reset(bool active)
{
  if (active)
  {
    is_on = false;
  }

  if (channel0)
    channel0->reset(active);
  if (channel1)
    channel1->reset(active);
  if (channel2)
    channel2->reset(active);
}



vp::IoReqStatus Udma_periph::custom_req(vp::IoReq *req, uint64_t offset)
{
  return vp::IO_REQ_INVALID;
}



vp::IoReqStatus Udma_periph::req(vp::IoReq *req, uint64_t offset)
{
  if (!is_on)
  {
    //top->trace.warning("Trying to access periph while it is off (periph: %d)\n", id);
    //return vp::IO_REQ_INVALID;
    // TODO should dump the warning but the himax driver is buggy
    return vp::IO_REQ_OK;
  }

  if (offset < UDMA_CHANNEL_TX_OFFSET)
  {
    if (channel0 == NULL)
    {
      top->trace.force_warning("Trying to access non-existing RX channel\n");
      return vp::IO_REQ_INVALID;
    }
    return channel0->req(req, offset);
  }
  else if (offset < UDMA_CHANNEL_CUSTOM_OFFSET)
  {
    if (channel1 == NULL)
    {
      top->trace.force_warning("Trying to access non-existing TX channel\n");
      return vp::IO_REQ_INVALID;
    }
    return channel1->req(req, offset - UDMA_CHANNEL_TX_OFFSET);
  }
  else
  {
    return custom_req(req, offset - UDMA_CHANNEL_CUSTOM_OFFSET);
  }

  return vp::IO_REQ_OK;
}



// bool Udma_transfer::prepare_req(vp::IoReq *req)
// {
//   req->prepare();
//   // The UDMA is dropping the address LSB to always have 32 bits aligned
//   // requests
//   req->set_addr(current_addr & ~0x3);
//   // The UDMA always sends 32 bits requests to L2 whatever the remaining size
//   req->set_size(4);

//   *(Udma_channel **)req->arg_get(0) = channel;
//   req->set_actual_size(remaining_size > 4 ? 4 : remaining_size);

//   current_addr += 4;
//   remaining_size -= 4;

//   return remaining_size <= 0;
// }
/* The UDMA cannot do misaligned access. It uses strobes to mask the byte to not overwrite the memory and split the request*/
bool Udma_transfer::prepare_req(vp::IoReq *req)
{
  req->prepare();
  // The UDMA has current_addr always aligned to 32-bit word. The model not
  req->set_addr(current_addr);
  req->set_size(4);

  *(Udma_channel **)req->arg_get(0) = channel;
  req->set_actual_size(remaining_size > 4 ? 4 : remaining_size);

  current_addr += 4;
  remaining_size -= 4;

  return remaining_size <= 0;
}

void udma::trigger_event(int event)
{
  trace.msg("Triggering event (event: %d)\n", event);
  event_itf.sync(event);
}


udma::udma(vp::ComponentConf &config)
: vp::Component(config)
{
  traces.new_trace("trace", &trace, vp::DEBUG);

  in.set_req_meth(&udma::req);
  new_slave_port("input", &in);

  this->periph_clock_itf.set_reg_meth(&udma::clk_reg);
  new_slave_port("periph_clock", &this->periph_clock_itf);

  nb_periphs = get_js_config()->get_child_int("nb_periphs");
  periphs.resize(nb_periphs);

  l2_read_fifo_size = get_js_config()->get_child_int("properties/l2_read_fifo_size");

  l2_itf.set_resp_meth(&udma::l2_response);
  l2_itf.set_grant_meth(&udma::l2_grant);
  new_master_port("l2_itf", &l2_itf);

  new_master_port("event_itf", &event_itf);

  event = event_new(udma::event_handler);

  l2_read_reqs = new Udma_queue<vp::IoReq>(l2_read_fifo_size);
  l2_write_reqs = new Udma_queue<vp::IoReq>(0);
  l2_read_waiting_reqs = new Udma_queue<vp::IoReq>(l2_read_fifo_size);
  for (int i=0; i<l2_read_fifo_size; i++)
  {
    vp::IoReq *req = new vp::IoReq();
    req->set_data(new uint8_t[4]);
    req->set_is_write(false);
    req->arg_alloc(); // Used to store channel;
    l2_read_reqs->push(req);
  }

  ready_rx_channels = new Udma_queue<Udma_channel>(nb_periphs);
  ready_tx_channels = new Udma_queue<Udma_channel>(nb_periphs);

  for (int i=0; i<nb_periphs; i++)
    periphs[i] = NULL;

  trace.msg("Instantiating udma channels (nb_periphs: %d)\n", nb_periphs);

  js::Config *interfaces = get_js_config()->get("interfaces");

  for (int i=0; i<interfaces->get_size(); i++)
  {
    std::string name = interfaces->get_elem(i)->get_str();
    js::Config *interface = get_js_config()->get(name);

    if (interface == NULL)
    {
      //warning.warning("Invalid JSON config, didn't find interface description (name: )\n");
      return;
    }

    int nb_channels = interface->get("nb_channels")->get_int();
    js::Config *ids = interface->get("ids");
    js::Config *offsets = interface->get("offsets");
    int size = interface->get_child_int("size");
    int version = interface->get("version")->get_int();

    trace.msg("Instantiating interface (type: %s, nb_channels: %d, version: %d)\n", name.c_str(), nb_channels, version);

    for (int j=0; j<nb_channels; j++)
    {
      int id = ids->get_elem(j)->get_int();
      int offset = offsets->get_elem(j)->get_int();


      if (0)
      {
      }
#ifdef HAS_SPIM
      else if (strcmp(name.c_str(), "spim") == 0)
      {
        trace.msg("Instantiating SPIM channel (id: %d, offset: 0x%x)\n", id, offset);
        if (version == 3)
        {
          Spim_periph_v3 *periph = new Spim_periph_v3(this, id, j);
          periphs[id] = periph;
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#endif
      else if (strcmp(name.c_str(), "uart") == 0)
      {
        trace.msg("Instantiating UART channel (id: %d, offset: 0x%x)\n", id, offset);
        if (version == 1)
        {
          Uart_periph_v1 *periph = new Uart_periph_v1(this, id, j);
          periphs[id] = periph;
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#ifdef HAS_HYPER
      else if (strcmp(name.c_str(), "hyper") == 0)
      {
        trace.msg("Instantiating HYPER channel (id: %d, offset: 0x%x)\n", id, offset);
        // if (version == 2)
        // {
        //   Hyper_periph_v2 *periph = new Hyper_periph_v2(this, id, j);
        //   periphs[id] = periph;
        // }
        if (version == 3)
        {
          Hyper_periph_v3 *periph = new Hyper_periph_v3(this, id, j);
          periphs[id] = periph;
          for(int channel_count=1; channel_count<=HYPER_NB_CHANNELS; channel_count++)
          {
            periphs[id+channel_count] = periph;
          }
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#endif
#ifdef HAS_MRAM
      else if (strcmp(name.c_str(), "mram") == 0)
      {
        trace.msg("Instantiating MRAM channel (id: %d, offset: 0x%x)\n", id, offset);
        if (version == 1)
        {
          Mram_periph_v1 *periph = new Mram_periph_v1(this, id, j);
          periphs[id] = periph;
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#endif
#ifdef HAS_I2C
      else if (strcmp(name.c_str(), "i2c") == 0)
      {
        trace.msg("Instantiating I2C channel (id: %d, offset: 0x%x)\n", id, offset);
        if (version == 2)
        {
          I2c_periph_v2 *periph = new I2c_periph_v2(this, id, j);
          periphs[id] = periph;
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#endif
#ifdef HAS_CPI
      else if (strcmp(name.c_str(), "cpi") == 0)
      {
        trace.msg("Instantiating CPI channel (id: %d, offset: 0x%x)\n", id, offset);
        if (version == 1)
        {
          Cpi_periph_v1 *periph = new Cpi_periph_v1(this, id, j);
          periphs[id] = periph;
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#endif
#ifdef I2S_VERSION
      else if (strcmp(name.c_str(), "i2s") == 0)
      {
        trace.msg("Instantiating I2S channel (version: %d, id: %d, offset: 0x%x)\n", I2S_VERSION, id, offset);
        if (version == I2S_VERSION)
        {
          I2s_periph *periph = new I2s_periph(this, id, j);

          if (size)
          {
            int periph_size = size;
            while(periph_size)
            {
              periphs[id] = periph;
              id++;
              periph_size -= UDMA_PERIPH_AREA_SIZE;
            }
          }
          else
          {
            periphs[id] = periph;
          }
        }
        else
        {
          throw logic_error("Non-supported udma version: " + std::to_string(version));
        }
      }
#endif
      else
      {
        trace.msg("Instantiating channel (id: %d, offset: 0x%x)\n", id, offset);
      }
    }
  }

}



void udma::push_l2_write_req(vp::IoReq *req)
{
  this->l2_write_reqs->push(req);
  this->check_state();
}


void udma::channel_handler(vp::Block *__this, vp::ClockEvent *event)
{
  Udma_channel *channel = (Udma_channel *)__this;
  channel->event_handler();
}



void udma::enqueue_ready(Udma_channel *channel)
{
  if (channel->is_tx())
    ready_tx_channels->push(channel);
  else
    channel->handle_ready();

  check_state();
}

void udma::event_handler(vp::Block *__this, vp::ClockEvent *event)
{
  udma *_this = (udma *)__this;

  if (!_this->l2_write_reqs->is_empty())
  {
    vp::IoReq *req = _this->l2_write_reqs->pop();
    _this->trace.msg("Sending write request to L2 (value: 0x%x, addr: 0x%x, size: 0x%x)\n", *(uint32_t *)req->get_data(), req->get_addr(), req->get_size());
    int err = _this->l2_itf.req(req);
    if (err == vp::IO_REQ_OK)
    {
    }
    else
    {
      _this->trace.warning("UNIMPLEMENTED AT %s %d\n", __FILE__, __LINE__);
    }

  }

  if (!_this->ready_tx_channels->is_empty() && !_this->l2_read_reqs->is_empty())
  {
    vp::IoReq *req = _this->l2_read_reqs->pop();
    Udma_channel *channel = _this->ready_tx_channels->pop();
    if (!channel->prepare_req(req))
    {
      _this->ready_tx_channels->push(channel);
    }

    _this->trace.msg("Sending read request to L2 (addr: 0x%x, size: 0x%x)\n", req->get_addr(), req->get_size());
    int err = _this->l2_itf.req(req);
    if (err == vp::IO_REQ_OK)
    {
      _this->trace.msg("Read FIFO received word from L2 (value: 0x%x)\n", *(uint32_t *)req->get_data());
      req->set_latency(req->get_latency() + _this->clock.get_cycles() + 1);
      _this->l2_read_waiting_reqs->push_from_latency(req);
    }
    else
    {
      _this->trace.warning("UNIMPLEMENTED AT %s %d\n", __FILE__, __LINE__);
    }
  }

  vp::IoReq *req = _this->l2_read_waiting_reqs->get_first();
  while (req != NULL && req->get_latency() <= _this->clock.get_cycles())
  {
    _this->trace.msg("Read request is ready, pushing to channel (req: %p)\n", req);

    Udma_channel *channel = *(Udma_channel **)req->arg_get(0);
    _this->l2_read_waiting_reqs->pop();
    channel->push_ready_req(req);

    req = _this->l2_read_waiting_reqs->get_first();
  }

  _this->check_state();
}


void udma::free_read_req(vp::IoReq *req)
{
  l2_read_reqs->push(req);
  check_state();
}

void udma::check_state()
{
  if ((!ready_tx_channels->is_empty() && !l2_read_reqs->is_empty()) || !l2_write_reqs->is_empty())
  {
    //printf("Enqueue 1 cycles\n");
    event_reenqueue_ext(event, 1);
  }

  if (!l2_read_waiting_reqs->is_empty())
  {
    //printf("Enqueue %ld cycles\n", l2_read_waiting_reqs->get_first()->get_latency() - clock.get_cycles());
    event_reenqueue_ext(event, l2_read_waiting_reqs->get_first()->get_latency() - clock.get_cycles());
  }
}

vp::IoReqStatus udma::conf_req(vp::IoReq *req, uint64_t offset)
{
  uint32_t *data = (uint32_t *)req->get_data();
  uint64_t size = req->get_size();
  bool is_write = req->get_is_write();

  if (offset == UDMA_CONF_CG_OFFSET)
  {
    if (is_write)
    {
      trace.msg("Writing clock-enable register (current_value: 0x%x, new_value: 0x%x)\n",
        clock_gating, *data);
      clock_gating = *data;
      for (int i=0; i<nb_periphs; i++)
      {
        if (periphs[i] != NULL && periphs[i]->id == i)
          periphs[i]->clock_gate((clock_gating >> i) & 1);
      }
    }
    else
      *data = clock_gating;

    return vp::IO_REQ_OK;
  }
  else if (offset == UDMA_CONF_EVTIN_OFFSET)
  {
    trace.msg("Unimplemented at %s %d\n", __FILE__, __LINE__);
  }

  return vp::IO_REQ_INVALID;
}



vp::IoReqStatus udma::periph_req(vp::IoReq *req, uint64_t offset)
{
  uint32_t *data = (uint32_t *)req->get_data();
  uint64_t size = req->get_size();
  bool is_write = req->get_is_write();

  if (size != 4) return vp::IO_REQ_INVALID;

  int periph_id = UDMA_PERIPH_GET(offset);

#if HAS_HYPER
  if (periph_id >= nb_periphs + HYPER_NB_CHANNELS || periphs[periph_id] == NULL)
#else
  if (periph_id >= nb_periphs || periphs[periph_id] == NULL)
#endif
  {
    trace.force_warning("Accessing invalid periph (id: %d)\n", periph_id);
    return vp::IO_REQ_INVALID;
  }

  int real_id = periphs[periph_id]->id;

  return periphs[periph_id]->req(req, offset - (UDMA_PERIPH_OFFSET(real_id) - UDMA_FIRST_CHANNEL_OFFSET));
}



vp::IoReqStatus udma::req(vp::Block *__this, vp::IoReq *req)
{
  udma *_this = (udma *)__this;

  uint64_t offset = req->get_addr();
  uint32_t *data = (uint32_t *)req->get_data();
  uint64_t size = req->get_size();
  bool is_write = req->get_is_write();

  _this->trace.msg("UDMA access (offset: 0x%x, size: 0x%x, is_write: %d)\n", offset, size, is_write);

  if (offset < UDMA_FIRST_CHANNEL_OFFSET)
  {
    return _this->conf_req(req, offset - UDMA_CONF_OFFSET);
  }
  else if (offset >= UDMA_FIRST_CHANNEL_OFFSET)
  {
    return _this->periph_req(req, offset - UDMA_FIRST_CHANNEL_OFFSET);
  }

  return vp::IO_REQ_INVALID;
}

void udma::l2_grant(vp::Block *__this, vp::IoReq *req)
{
  udma *_this = (udma *)__this;
  _this->trace.warning("UNIMPLEMENTED AT %s %d\n", __FILE__, __LINE__);
}

void udma::l2_response(vp::Block *__this, vp::IoReq *req)
{
  udma *_this = (udma *)__this;
  _this->trace.warning("UNIMPLEMENTED AT %s %d\n", __FILE__, __LINE__);
}

void udma::clk_reg(Component *__this, Component *clock)
{
  udma *_this = (udma *)__this;
  _this->periph_clock = (vp::ClockEngine *)clock;
}

void udma::reset(bool active)
{
  if (active)
  {
    clock_gating = 0;
  }

  for (int i=0; i<nb_periphs; i++)
  {
    if (periphs[i] != NULL && periphs[i]->id == i)
      periphs[i]->reset(active);
  }

}





template<class T>
void Udma_queue<T>::push_from_latency(T *cmd)
{
  T *current = first, *prev = NULL;
  while (current && cmd->get_latency() > current->get_latency())
  {
    prev = current;
    current = current->get_next();
  }

  if (current == NULL)
    last = cmd;

  if (prev)
    prev->set_next(cmd);
  else
    first = cmd;
  cmd->set_next(current);
  nb_cmd++;
}




extern "C" vp::Component *gv_new(vp::ComponentConf &config)
{
  return new udma(config);
}
