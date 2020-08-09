/*
 * Copyright (C) 2020 Robin Gareus <robin@gareus.org>
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

#include <glibmm/main.h>

#include "ardour/plugin_insert.h"
#include "ardour/vst3_plugin.h"

#include "gtkmm2ext/gui_thread.h"

#include "vst3_plugin_ui.h"

using namespace ARDOUR;
using namespace Steinberg;

VST3PluginUI::VST3PluginUI (boost::shared_ptr<PluginInsert> pi, boost::shared_ptr<VST3Plugin> vst3)
	: PlugUIBase (pi)
	, _pi (pi)
	, _vst3 (vst3)
	, _req_width (0)
	, _req_height (0)
{
	_ardour_buttons_box.set_spacing (6);
	_ardour_buttons_box.set_border_width (6);
	_ardour_buttons_box.pack_end (focus_button, false, false);
	_ardour_buttons_box.pack_end (bypass_button, false, false, 4);

	/* TODO Presets */

	_ardour_buttons_box.pack_end (pin_management_button, false, false);
	_ardour_buttons_box.pack_start (latency_button, false, false, 4);

	_vst3->OnResizeView.connect (_resize_connection, invalidator (*this), boost::bind (&VST3PluginUI::resize_callback, this, _1, _2), gui_context());

	pack_start (_ardour_buttons_box, false, false);
	_ardour_buttons_box.show_all ();
}

VST3PluginUI::~VST3PluginUI ()
{
	IPlugView* view = _vst3->view ();
	if (view) {
		view->removed ();
	}
}

gint
VST3PluginUI::get_preferred_height ()
{
	IPlugView* view = _vst3->view ();
	ViewRect rect;
	if (view && view->getSize (&rect) == kResultOk){
		return rect.bottom - rect.top;
	}
	return 0;
}

gint
VST3PluginUI::get_preferred_width ()
{
	IPlugView* view = _vst3->view ();
	ViewRect rect;
	if (view && view->getSize (&rect) == kResultOk){
		return rect.right - rect.left;
	}
	return 0;
}

bool
VST3PluginUI::resizable ()
{
	IPlugView* view = _vst3->view ();
	return view && view->canResize () == kResultTrue;
}
