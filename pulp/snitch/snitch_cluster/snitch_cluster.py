#
# Copyright (C) 2020 ETH Zurich and University of Bologna
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

import gvsoc.runner
import pulp.snitch.snitch_core as iss
import memory.memory as memory
import interco.router as router
import gvsoc.systree
import pulp.snitch.snitch_cluster.cluster_registers
import pulp.snitch.snitch_cluster.spatz.cluster_registers
from pulp.snitch.snitch_cluster.dma_interleaver import DmaInterleaver
from pulp.snitch.zero_mem import ZeroMem
from elftools.elf.elffile import *
from pulp.idma.snitch_dma import SnitchDma
from pulp.cluster.l1_interleaver import L1_interleaver
import gvsoc.runner
import math
from pulp.snitch.sequencer import Sequencer
from pulp.snitch.hierarchical_cache import Hierarchical_cache



class Area:

    def __init__(self, base, size):
        self.base = base
        self.size = size



class ClusterArch:
    def __init__(self, properties, base, first_hartid, auto_fetch=False, boot_addr=0x0000_1000):
        self.nb_core = properties.nb_core_per_cluster
        self.base = base
        self.first_hartid = first_hartid

        self.boot_addr = boot_addr
        self.auto_fetch = auto_fetch
        self.barrier_irq = 19
        self.tcdm          = ClusterArch.Tcdm(base, self.nb_core)
        self.peripheral    = Area( base + 0x0002_0000, 0x0001_0000)
        self.zero_mem      = Area( base + 0x0003_0000, 0x0001_0000)
        self.core_type = properties.core_type
        self.use_spatz = properties.use_spatz
        self.isa = properties.isa

    class Tcdm:
        def __init__(self, base, nb_masters):
            self.area = Area( base + 0x0000_0000, 0x0002_0000)
            self.nb_banks_per_superbank = 8
            self.bank_width = 8
            self.nb_superbanks = 4
            self.bank_size = self.area.size / self.nb_superbanks / self.nb_banks_per_superbank
            self.nb_masters = nb_masters


class SnitchClusterTcdm(gvsoc.systree.Component):

    def __init__(self, parent, name, arch):
        super().__init__(parent, name)

        banks = []
        nb_banks = arch.nb_superbanks * arch.nb_banks_per_superbank
        for i in range(0, nb_banks):
            banks.append(memory.Memory(self, f'bank_{i}', size=arch.bank_size, atomics=True,
                width_log2=int(math.log2(arch.bank_width)), latency=0))

        interleaver = L1_interleaver(self, 'interleaver', nb_slaves=nb_banks,
            nb_masters=arch.nb_masters, interleaving_bits=int(math.log2(arch.bank_width)))

        dma_interleaver = DmaInterleaver(self, 'dma_interleaver', arch.nb_masters,
            nb_banks, arch.bank_width)

        for i in range(0, nb_banks):
            self.bind(interleaver, 'out_%d' % i, banks[i], 'input')
            self.bind(dma_interleaver, 'out_%d' % i, banks[i], 'input')

        for i in range(0, arch.nb_masters):
            self.bind(self, f'in_{i}', interleaver, f'in_{i}')
            self.bind(self, f'dma_input', dma_interleaver, f'input')

    def i_INPUT(self, port: int) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'in_{port}', signature='io')

    def i_DMA_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'dma_input', signature='io')



class SnitchCluster(gvsoc.systree.Component):

    def __init__(self, parent, name, arch, parser=None, entry=0, auto_fetch=True, binaries=None):
        super().__init__(parent, name)

        #
        # Components
        #

        # Main router
        wide_axi = router.Router(self, 'wide_axi', bandwidth=64)
        narrow_axi = router.Router(self, 'narrow_axi', bandwidth=8)

        # Dedicated router for dma to TCDM
        tcdm_dma_ico = router.Router(self, 'tcdm_dma_ico', bandwidth=64)

        # L1 Memory
        tcdm = SnitchClusterTcdm(self, 'tcdm', arch.tcdm)

        # Zero memory
        zero_mem = ZeroMem(self, 'zero_mem', size=arch.zero_mem.size)

        # Shared icache
        icache = Hierarchical_cache(self, 'icache', nb_cores=arch.nb_core, has_cc=0,
            l1_line_size_bits=7)

        # Cores
        cores = []
        fp_cores = []
        cores_ico = []
        xfrep = True
        if xfrep:
            fpu_sequencers = []

        binary = None
        binaries = []
        if parser is not None:
            [args, __] = parser.parse_known_args()

            if parser is not None:
                [args, otherArgs] = parser.parse_known_args()
                binary = args.binary
                if binary is not None:
                    binaries = [binary]

        for core_id in range(0, arch.nb_core):

            if arch.core_type == 'fast' or arch.use_spatz:
                cores.append(iss.SnitchFast(self, f'pe{core_id}', isa=arch.isa,
                    fetch_enable=arch.auto_fetch, boot_addr=arch.boot_addr,
                    core_id=arch.first_hartid + core_id, htif=True, binaries=binaries,
                    inc_spatz=arch.use_spatz
                ))

            else:
                cores.append(iss.Snitch(self, f'pe{core_id}', isa=arch.isa,
                    fetch_enable=arch.auto_fetch, boot_addr=arch.boot_addr,
                    core_id=arch.first_hartid + core_id, htif=True, binaries=binaries))

                fp_cores.append(iss.Snitch_fp_ss(self, f'fp_ss{core_id}', isa=arch.isa,
                    fetch_enable=arch.auto_fetch, boot_addr=arch.boot_addr,
                    core_id=arch.first_hartid + core_id, htif=True, binaries=binaries))
                if xfrep:
                    fpu_sequencers.append(Sequencer(self, f'fpu_sequencer{core_id}', latency=0))

            cores_ico.append(router.Router(self, f'pe{core_id}_ico', bandwidth=arch.tcdm.bank_width))

        # Cluster peripherals
        if arch.use_spatz:
            cluster_registers = pulp.snitch.snitch_cluster.spatz.cluster_registers.ClusterRegisters(
                self, 'cluster_registers', nb_cores=arch.nb_core, boot_addr=entry)
        else:
            cluster_registers = pulp.snitch.snitch_cluster.cluster_registers.ClusterRegisters(
                self, 'cluster_registers', nb_cores=arch.nb_core, boot_addr=entry)

        # Cluster DMA
        idma = SnitchDma(self, 'idma', loc_base=arch.tcdm.area.base, loc_size=arch.tcdm.area.size,
            tcdm_width=4096, transfer_queue_size=8, burst_queue_size=24)

        #
        # Bindings
        #

        # Narrow router for cores data accesses
        self.o_NARROW_INPUT(narrow_axi.i_INPUT())
        narrow_axi.o_MAP(self.i_NARROW_SOC())
        # TODO check on real HW where this should go. This probably go through wide axi to
        # have good bandwidth when transferring from one cluster to another
        narrow_axi.o_MAP(cores_ico[0].i_INPUT(), base=arch.tcdm.area.base, size=arch.tcdm.area.size, rm_base=False)

        # Wire router for DMA and instruction caches
        self.o_WIDE_INPUT(wide_axi.i_INPUT())
        wide_axi.o_MAP(self.i_WIDE_SOC())

        # Icache
        icache.o_REFILL( wide_axi.i_INPUT() )

        # Remote Access to TCDM
        wide_axi.o_MAP(tcdm.i_DMA_INPUT(), base=arch.tcdm.area.base, size=arch.tcdm.area.size, rm_base=True)

        # Cores
        cores[arch.nb_core-1].o_OFFLOAD(idma.i_OFFLOAD())
        idma.o_OFFLOAD_GRANT(cores[arch.nb_core-1].i_OFFLOAD_GRANT())

        # Cores
        for core_id in range(0, arch.nb_core):
            self.__o_FETCHEN( cores[core_id].i_FETCHEN() )

        for core_id in range(0, arch.nb_core):
            cores[core_id].o_BARRIER_REQ(cluster_registers.i_BARRIER_ACK(core_id))
        for core_id in range(0, arch.nb_core):
            cores[core_id].o_DATA(cores_ico[core_id].i_INPUT())
            cores_ico[core_id].o_MAP(tcdm.i_INPUT(core_id), base=arch.tcdm.area.base,
                size=arch.tcdm.area.size, rm_base=True)
            cores_ico[core_id].o_MAP(narrow_axi.i_INPUT())
            cores[core_id].o_FETCH(icache.i_INPUT(core_id))

            # Icache
            cores[core_id].o_FLUSH_CACHE(icache.i_FLUSH())
            icache.o_FLUSH_ACK(cores[core_id].i_FLUSH_CACHE_ACK())


        for core_id in range(0, arch.nb_core):
            if arch.core_type == 'accurate' and not arch.use_spatz:
                fp_cores[core_id].o_DATA( cores_ico[core_id].i_INPUT() )
                self.__o_FETCHEN( fp_cores[core_id].i_FETCHEN() )

                # SSR in fp subsystem datem mover <-> memory port
                self.bind(fp_cores[core_id], 'ssr_dm0', cores_ico[core_id], 'input')
                self.bind(fp_cores[core_id], 'ssr_dm1', cores_ico[core_id], 'input')
                self.bind(fp_cores[core_id], 'ssr_dm2', cores_ico[core_id], 'input')

                # Use WireMaster & WireSlave
                # Add fpu sequence buffer in between int core and fp core to issue instructions
                if xfrep:
                    self.bind(cores[core_id], 'acc_req', fpu_sequencers[core_id], 'input')
                    self.bind(fpu_sequencers[core_id], 'output', fp_cores[core_id], 'acc_req')
                    self.bind(cores[core_id], 'acc_req_ready', fpu_sequencers[core_id], 'acc_req_ready')
                    self.bind(fpu_sequencers[core_id], 'acc_req_ready_o', fp_cores[core_id], 'acc_req_ready')
                else:
                    # Comment out if we want to add sequencer
                    self.bind(cores[core_id], 'acc_req', fp_cores[core_id], 'acc_req')
                    self.bind(cores[core_id], 'acc_req_ready', fp_cores[core_id], 'acc_req_ready')

                self.bind(fp_cores[core_id], 'acc_rsp', cores[core_id], 'acc_rsp')

            else:
                self.bind(cores[core_id], 'ssr_dm0', cores_ico[core_id], 'input')
                self.bind(cores[core_id], 'ssr_dm1', cores_ico[core_id], 'input')
                self.bind(cores[core_id], 'ssr_dm2', cores_ico[core_id], 'input')


        # Cluster peripherals
        narrow_axi.o_MAP(cluster_registers.i_INPUT(), base=arch.peripheral.base,
            size=arch.peripheral.size, rm_base=True)
        for core_id in range(0, arch.nb_core):
            self.bind(cluster_registers, f'barrier_ack', cores[core_id], 'barrier_ack')
        for core_id in range(0, arch.nb_core):
            cluster_registers.o_EXTERNAL_IRQ(core_id, cores[core_id].i_IRQ(arch.barrier_irq))
            self.__o_MSIP(core_id, cores[core_id].i_IRQ(3))
            self.__o_MTIP(core_id, cores[core_id].i_IRQ(7))
            self.__o_MEIP(core_id, cores[core_id].i_IRQ(11))

        # Cluster DMA
        idma.o_AXI(wide_axi.i_INPUT())
        idma.o_TCDM(tcdm.i_DMA_INPUT())

        # Zero mem
        wide_axi.o_MAP(zero_mem.i_INPUT(), base=arch.zero_mem.base, size=arch.zero_mem.size, rm_base=True)
        narrow_axi.o_MAP(wide_axi.i_INPUT(), name='zero_mem', base=arch.zero_mem.base, size=arch.zero_mem.size, rm_base=False)

    def i_MEIP(self, core: int) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'meip_{core}', signature='wire<bool>')

    def __o_MEIP(self, core: int, itf: gvsoc.systree.SlaveItf):
        self.itf_bind(f'meip_{core}', itf, signature='wire<bool>', composite_bind=True)

    def i_MTIP(self, core: int) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'mtip_{core}', signature='wire<bool>')

    def __o_MTIP(self, core: int, itf: gvsoc.systree.SlaveItf):
        self.itf_bind(f'mtip_{core}', itf, signature='wire<bool>', composite_bind=True)

    def i_MSIP(self, core: int) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, f'msip_{core}', signature='wire<bool>')

    def __o_MSIP(self, core: int, itf: gvsoc.systree.SlaveItf):
        self.itf_bind(f'msip_{core}', itf, signature='wire<bool>', composite_bind=True)

    def i_FETCHEN(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'fetchen', signature='wire<bool>')

    def __o_FETCHEN(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('fetchen', itf, signature='wire<bool>', composite_bind=True)

    # Wide router for dma and instruction cache accesses
    def i_WIDE_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wide_input', signature='io')

    def o_WIDE_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wide_input', itf, signature='io', composite_bind=True)

    def i_WIDE_SOC(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'wide_soc', signature='io')

    # Wide Output of the cluster
    def o_WIDE_SOC(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('wide_soc', itf, signature='io')

    # Narrow router for cores data accesses
    def i_NARROW_INPUT(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'narrow_input', signature='io')

    def o_NARROW_INPUT(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('narrow_input', itf, signature='io', composite_bind=True)

    def i_NARROW_SOC(self) -> gvsoc.systree.SlaveItf:
        return gvsoc.systree.SlaveItf(self, 'narrow_soc', signature='io')
    # Narrow Output of the cluster
    def o_NARROW_SOC(self, itf: gvsoc.systree.SlaveItf):
        self.itf_bind('narrow_soc', itf, signature='io')
