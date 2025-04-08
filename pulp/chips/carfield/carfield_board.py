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
from pulp.chips.carfield.soc import Soc

class Carfield_board(st.Component):

    def __init__(self, parent, name, parser, options):

        super(Carfield_board, self).__init__(parent, name, options=options)

        parser.add_argument("--isa", dest="isa", type=str, default="rv64imafdc",
            help="RISCV-V ISA string (default: %(default)s)")

        parser.add_argument("--arg", dest="args", action="append",
            help="Specify application argument (passed to main)")

        [args, otherArgs] = parser.parse_known_args()
        debug_binaries = []
        if args.binary is not None:
            debug_binaries.append(args.binary)

        clock = Clock_domain(self, 'clock', frequency=10000000)

        soc = Soc(self, 'soc', args, debug_binaries)

        self.bind(clock, 'out', soc, 'clock')