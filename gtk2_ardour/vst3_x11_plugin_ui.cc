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

#ifdef WAF_BUILD
#include "gtk2ardour-config.h"
#endif

#include <boost/unordered_map.hpp>

#include <glibmm/main.h>
#include <gtkmm/socket.h>

#include "ardour/plugin_insert.h"
#include "ardour/vst3_plugin.h"

#include "gtkmm2ext/gui_thread.h"

#include "vst3_x11_plugin_ui.h"

#include <gdk/gdkx.h> /* must come later than glibmm/object.h */

using namespace ARDOUR;
using namespace Steinberg;

class VST3X11Runloop : public Linux::IRunLoop
{
private:
	struct EventHandler
	{
		EventHandler (Linux::IEventHandler* handler = 0, GIOChannel* gio_channel = 0)
			: _handler (handler)
			, _gio_channel (gio_channel)
		{}
		bool operator== (EventHandler const& other) {
			return other._handler == _handler && other._gio_channel == _gio_channel;
		}
		Linux::IEventHandler* _handler;
		GIOChannel*           _gio_channel;
	};

	boost::unordered_map<FileDescriptor, EventHandler> _event_handlers;
	boost::unordered_map<guint, Linux::ITimerHandler*> _timer_handlers;

	static gboolean event (GIOChannel* source, GIOCondition condition, gpointer data)
	{
		(void)condition;
		Linux::IEventHandler* handler = reinterpret_cast<Linux::IEventHandler*> (data);
		handler->onFDIsSet (g_io_channel_unix_get_fd (source));
		return false;
	}

	static gboolean timeout (gpointer data)
	{
		Linux::ITimerHandler* handler = reinterpret_cast<Linux::ITimerHandler*> (data);
		handler->onTimer ();
		return true;
	}

public:
	~VST3X11Runloop ()
	{
		for (boost::unordered_map<FileDescriptor, EventHandler>::iterator it = _event_handlers.begin (); it != _event_handlers.end (); ++it) {
			g_io_channel_unref (it->second._gio_channel);
		}
		for (boost::unordered_map<guint, Linux::ITimerHandler*>::iterator it = _timer_handlers.begin (); it != _timer_handlers.end (); ++it) {
			g_source_remove (it->first);
		}
	}

	/* VST3 IRunLoop interface */
	tresult registerEventHandler (Linux::IEventHandler* handler, FileDescriptor fd) SMTG_OVERRIDE
	{
		GIOChannel* gio_channel = g_io_channel_unix_new (fd);
		g_io_add_watch (gio_channel, (GIOCondition) (G_IO_IN | G_IO_OUT | G_IO_ERR | G_IO_HUP), event, handler);

		_event_handlers[fd] = EventHandler (handler, gio_channel);
		return kResultTrue;
	}

	tresult unregisterEventHandler (Linux::IEventHandler* handler) SMTG_OVERRIDE
	{
		if (!handler) {
			return kInvalidArgument;
		}

		for (boost::unordered_map<FileDescriptor, EventHandler>::iterator it = _event_handlers.begin (); it != _event_handlers.end (); ++it) {
			if (it->second._handler == handler) {
				g_io_channel_unref (it->second._gio_channel);
				_event_handlers.erase (it);
				return kResultTrue;
			}
		}
		return kResultFalse;
	}

	tresult registerTimer (Linux::ITimerHandler* handler, TimerInterval milliseconds) SMTG_OVERRIDE
	{
		if (!handler || milliseconds == 0) {
			return kInvalidArgument;
		}
		guint id = g_timeout_add (milliseconds, timeout, handler);
		_timer_handlers[id] = handler;
		return kResultTrue;

	}

	tresult unregisterTimer (Linux::ITimerHandler* handler) SMTG_OVERRIDE
	{
		if (!handler) {
			return kInvalidArgument;
		}

		for (boost::unordered_map<guint, Linux::ITimerHandler*>::iterator it = _timer_handlers.begin (); it != _timer_handlers.end (); ++it) {
			if (it->second == handler) {
				g_source_remove (it->first);
				_timer_handlers.erase (it);
				return kResultTrue;
			}
		}
		return kResultFalse;
	}

	uint32 PLUGIN_API addRef () SMTG_OVERRIDE { return 1; }
	uint32 PLUGIN_API release () SMTG_OVERRIDE { return 1; }
	tresult queryInterface (const TUID, void**) SMTG_OVERRIDE { return kNoInterface; }
};

VST3X11PluginUI::VST3X11PluginUI (boost::shared_ptr<PluginInsert> pi, boost::shared_ptr<VST3Plugin> vst3)
	: VST3PluginUI (pi, vst3)
	, _runloop (new VST3X11Runloop)
{
	_vst3->set_runloop (_runloop.get ());

	pack_start (_gui_widget, true, true);

	_gui_widget.signal_realize().connect (mem_fun (this, &VST3X11PluginUI::view_realized));
	_gui_widget.signal_size_request ().connect (mem_fun (this, &VST3X11PluginUI::view_size_request));
	_gui_widget.signal_size_allocate ().connect (mem_fun (this, &VST3X11PluginUI::view_size_allocate));

	_gui_widget.show ();
}

VST3X11PluginUI::~VST3X11PluginUI ()
{
}

void
VST3X11PluginUI::view_realized ()
{
	IPlugView* view = _vst3->view ();
	Window window = _gui_widget.get_id ();
	if (kResultOk != view->attached (reinterpret_cast<void*> (window), "X11EmbedWindowID")) {
		assert (0);
	}

	ViewRect rect;
	if (view->getSize (&rect) == kResultOk) {
		_req_width  = rect.right - rect.left;
		_req_height = rect.bottom - rect.top;
	}
}

void
VST3X11PluginUI::view_size_request (GtkRequisition* requisition)
{
	requisition->width  = _req_width;
	requisition->height = _req_height;
}

void
VST3X11PluginUI::view_size_allocate (Gtk::Allocation& allocation)
{
	IPlugView* view = _vst3->view ();
	if (!view) {
		return;
	}
	ViewRect rect;
	if (view->getSize (&rect) == kResultOk) {
		rect.right = rect.left + allocation.get_width ();
		rect.bottom = rect.top + allocation.get_height ();
#if 0
		if (view->checkSizeConstraint (&rect) != kResultTrue) {
			view->getSize (&rect);
		}
		allocation.set_width (rect.right - rect.left);
		allocation.set_height (rect.bottom - rect.top);
#endif
		view->onSize (&rect);
	}
}

void
VST3X11PluginUI::resize_callback (int width, int height)
{
	//printf ("VST3X11PluginUI::resize_callback %d x %d\n", width, height);
}

bool
VST3X11PluginUI::start_updating (GdkEventAny*)
{
	return false;
}

bool
VST3X11PluginUI::stop_updating (GdkEventAny*)
{
	return false;
}

int
VST3X11PluginUI::package (Gtk::Window&)
{
	return 0;
}

bool
VST3X11PluginUI::on_window_show (const std::string& /*title*/)
{
	IPlugView* view = _vst3->view ();
	if (!view) {
		return false;
	}
	gtk_widget_realize (GTK_WIDGET(_gui_widget.gobj()));
	_gui_widget.show ();
	return true;
}

void
VST3X11PluginUI::on_window_hide ()
{
	_gui_widget.hide ();
}

void
VST3X11PluginUI::grab_focus ()
{
#if 0
	IPlugView* view = _vst3->view ();
	if (view) {
		view->onFocus (true);
	}
#endif
}
