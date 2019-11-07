/*
 * Copyright (C) 2019 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef _ardour_vst3_scan_h_
#define _ardour_vst3_scan_h_

#include <vector>
#include <boost/shared_ptr.hpp>

#include "ardour/libardour_visibility.h"

namespace ARDOUR {
class VST3PluginModule;
}

namespace ARDOUR {

struct VST3Info {
	VST3Info ()
		: index (0)
		, n_inputs (0)
		, n_outputs (0)
		, n_aux_inputs (0)
		, n_aux_outputs (0)
		, n_midi_inputs (0)
		, n_midi_outputs (0)
	{}

	int         index;
	std::string uid;
	std::string name;
	std::string vendor;
	std::string category;
	std::string version;
	std::string sdkVersion;

	int n_inputs;
	int n_outputs;
	int n_aux_inputs;
	int n_aux_outputs;
	int n_midi_inputs;
	int n_midi_outputs;
};

LIBARDOUR_API extern bool
discover_vst3 (boost::shared_ptr<VST3PluginModule>,
               std::vector<VST3Info>&);

} // namespace ARDOUR

#endif
