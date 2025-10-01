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

import gvsoc.systree as st
from vp.clock_domain import Clock_domain
from pulp.chips.carfield.hostd.soc import Soc as Carfield_soc
from pulp.chips.pulp_open.cluster import Cluster as Pulpd_cluster
from devices.hyperbus.hyperram import Hyperram

class Carfield_board(st.Component):

    def __init__(self, parent, name, parser, options):
        super(Carfield_board, self).__init__(parent, name, options=options)

        #
        # Config
        #
        cluster_config_file='pulp/chips/carfield/pulpd/cluster.json'
        # TODO: use soc config file 

        # Clock domains
        soc_clock = Clock_domain(self, 'soc_clock_domain', frequency=10000000)
        pulpd_clock = Clock_domain(self, 'pulpd_clock_domain', frequency=10000000)

        #
        # Memory Infrastructure
        #

        # HyperRAM TODO: complete interconnect
        hyperram = Hyperram(self, 'ddr') 
 

        #
        # Domains
        #
        
        # PULP Domain (PULPD) - Integer cluster with RedMule accelerator
        pulpd = Pulpd_cluster(self, 'pulpd', config_file=cluster_config_file, cid=0)
        
        # Host Domain (HOSTD) - CVA6-based Cheshire host domain
        hostd = Carfield_soc(self, 'hostd', parser, chip=self, pulp_cluster=pulpd)
        
        # Spatz Domain (SPATZD) - Vectorial PMCA with Snitch+Spatz cores
        #TODO: add Spatz domain


        #
        # Bindings
        #

        # Clock domain bindings
        self.bind(soc_clock, 'out', hostd, 'clock')
        self.bind(soc_clock, 'out', hyperram, 'clock')
        self.bind(pulpd_clock, 'out', pulpd, 'clock')

        # Host domain interconnect bindings
        #self.bind(hostd, 'system_axi', system_axi, 'input')
        self.bind(hostd, 'to_pulpd', pulpd, 'input')
    
        # PULP domain interconnect bindings
        #self.bind(pulpd, 'system_axi', system_axi, 'input')
        #self.bind(pulpd, 'dma_irq', hostd, 'pulpd_dma_irq')
        self.bind(pulpd, 'soc', hostd, 'soc_input')