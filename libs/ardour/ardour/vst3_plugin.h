/*
 * Copyright (C) 2019-2020 Robin Gareus <robin@gareus.org>
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

#ifndef _ardour_vst3_plugin_h_
#define _ardour_vst3_plugin_h_

#include "pbd/signals.h"

#include "ardour/plugin.h"
#include "ardour/vst3_host.h"

namespace ARDOUR {
class VST3PluginModule;
}

#if defined(__clang__)
#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wnon-virtual-dtor"
#elif __GNUC__ > 4 || (__GNUC__ == 4 && __GNUC_MINOR__ >= 6)
#    pragma GCC diagnostic push
#    pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

namespace Steinberg {

/* VST3 hosted Plugin abstraction Implementation
 *
 * For convenience this is placed in the Steinberg namespace.
 * Ardour::VST3Plugin has-a VST3PI (not is-a).
 */
class LIBARDOUR_API VST3PI
	: public Vst::IComponentHandler
	, public Vst::IConnectionPoint
	, public IPlugFrame
{
public:
	VST3PI (boost::shared_ptr<ARDOUR::VST3PluginModule> m, int index, std::string unique_id);
	virtual ~VST3PI ();

	/* IComponentHandler */
	tresult beginEdit (Vst::ParamID id) SMTG_OVERRIDE;
	tresult performEdit (Vst::ParamID id, Vst::ParamValue value) SMTG_OVERRIDE;
	tresult endEdit (Vst::ParamID id) SMTG_OVERRIDE;
	tresult restartComponent (int32 flags) SMTG_OVERRIDE;

	/* IConnectionPoint API */
	tresult connect (Vst::IConnectionPoint* other) SMTG_OVERRIDE;
	tresult disconnect (Vst::IConnectionPoint* other) SMTG_OVERRIDE;
	tresult notify (Vst::IMessage* message) SMTG_OVERRIDE;

	/* GUI */
	tresult resizeView (IPlugView* view, ViewRect* newSize) SMTG_OVERRIDE;
	IPlugView* view ();
	PBD::Signal2<void, int, int> OnResizeView;
#if SMTG_OS_LINUX
	void set_runloop (Linux::IRunLoop*);
#endif

	tresult queryInterface (const TUID _iid, void** obj);
	uint32 addRef () SMTG_OVERRIDE { return 1; }
	uint32 release () SMTG_OVERRIDE { return 1; }

	/* API for Ardour -- Ports */
	uint32_t    designated_bypass_port () const { return _port_id_bypass; }
	uint32_t    parameter_count () const { return _ctrl_params.size (); }
	bool        parameter_is_automatable (uint32_t p) const { return _ctrl_params[p].automable; }
	bool        parameter_is_readonly (uint32_t p) const { return _ctrl_params[p].read_only; }
	std::string parameter_label (uint32_t p) const { return _ctrl_params[p].label; }
	float       default_value (uint32_t p) const;
	void        get_parameter_descriptor (uint32_t, ARDOUR::ParameterDescriptor&) const;
	std::string print_parameter (uint32_t p) const;

	uint32_t n_audio_inputs () const;
	uint32_t n_audio_outputs () const;

	/* MIDI/Event interface */
	void cycle_start ();
	void add_event (Evoral::Event<samplepos_t> const&, int32_t bus);

	uint32_t n_midi_inputs () const;
	uint32_t n_midi_outputs () const;

	/* API for Ardour -- Parameters */
	bool         set_parameter_by_id (Vst::ParamID id, float value);
	void         set_parameter (uint32_t p, float value, int32 sample_off);
	float        get_parameter (uint32_t p) const;
	std::string  format_parameter (uint32_t p) const;
	Vst::ParamID index_to_id (uint32_t) const;

	/* API for Ardour -- Setup/Processing */
	uint32_t  plugin_latency () { return _processor->getLatencySamples (); }
	bool      set_block_size (int32_t);
	bool      activate ();
	bool      deactivate ();

	Vst::ProcessContext& context () { return _context; }

	void process (float** ins, float** outs, uint32_t n_samples);

private:
	void init ();
	void terminate ();
	bool connect_components ();
	bool disconnect_components ();

	bool  update_processor ();
	int32 count_channels (Vst::MediaType, Vst::BusDirection, Vst::BusType) const;

	bool evoral_to_vst3 (Vst::Event&, Evoral::Event<samplepos_t> const&, int32_t);
	bool vst3_to_evoral (Evoral::Event<samplepos_t>&, Vst::Event const&);

	bool midi_controller (int32_t, int16_t, Vst::CtrlNumber, Vst::ParamID &id);

	boost::shared_ptr<ARDOUR::VST3PluginModule> _module;

	IPluginFactory*       _factory;
	Vst::IComponent*      _component;
	Vst::IEditController* _controller;
	IPlugView*            _view;
#if SMTG_OS_LINUX
	Linux::IRunLoop*      _run_loop;
#endif

	FUnknownPtr<Vst::IAudioProcessor> _processor;
	Vst::ProcessContext               _context;

	/* Parameters */
	Vst3ParameterChanges _input_param_changes;
	Vst3ParameterChanges _output_param_changes;

	Vst3EventList _input_events;
	Vst3EventList _output_events;

	/* state */
	bool    _is_processing;
	int32_t _block_size;

	/* ports */
	struct Param {
		uint32_t    id;
		std::string label;
		std::string unit;
		int32_t     steps; // 1: toggle
		double      normal;
		bool        is_enum;
		bool        read_only;
		bool        automable;
	};

	uint32_t                         _port_id_bypass;
	uint32_t                         _port_id_program_change;

	std::vector<Param>               _ctrl_params;
	std::map<Vst::ParamID, uint32_t> _ctrl_id_index;
	std::map<uint32_t, Vst::ParamID> _ctrl_index_id;
	std::vector<float>               _shadow_data;

	int _n_inputs;
	int _n_outputs;
	int _n_aux_inputs;
	int _n_aux_outputs;
	int _n_midi_inputs;
	int _n_midi_outputs;
};

} // namespace Steinberg

namespace ARDOUR {

class LIBARDOUR_API VST3Plugin : public ARDOUR::Plugin
{
public:
	VST3Plugin (AudioEngine&, Session&, Steinberg::VST3PI*);
	VST3Plugin (const VST3Plugin&);
	~VST3Plugin ();

	std::string unique_id () const { return get_info ()->unique_id; }
	const char* name ()      const { return get_info ()->name.c_str (); }
	const char* label ()     const { return get_info ()->name.c_str (); }
	const char* maker ()     const { return get_info ()->creator.c_str (); }

	uint32_t parameter_count () const;
	float    default_value (uint32_t port);
	void     set_parameter (uint32_t port, float val, sampleoffset_t when);
	float    get_parameter (uint32_t port) const;
	int      get_parameter_descriptor (uint32_t which, ParameterDescriptor&) const;
	uint32_t nth_parameter (uint32_t port, bool& ok) const;
	bool     print_parameter (uint32_t, std::string&) const;

	bool parameter_is_audio (uint32_t) const { return false; }
	bool parameter_is_control (uint32_t) const { return true; }
	bool parameter_is_input (uint32_t) const;
	bool parameter_is_output (uint32_t) const;

	uint32_t designated_bypass_port ();

	std::set<Evoral::Parameter> automatable () const;
	std::string describe_parameter (Evoral::Parameter);
	IOPortDescription describe_io_port (DataType dt, bool input, uint32_t id) const;
	PluginOutputConfiguration possible_output () const;

	std::string state_node_name () const { return "vst3"; }

	void add_state (XMLNode*) const;
	int  set_state (const XMLNode&, int version);

	bool        load_preset (PresetRecord);
	std::string do_save_preset (std::string);
	void        do_remove_preset (std::string);

	void activate ()   { _plug->activate (); }
	void deactivate () { _plug->deactivate (); }

	int set_block_size (pframes_t);

	int connect_and_run (BufferSet&  bufs,
	                     samplepos_t start, samplepos_t end, double speed,
	                     ChanMapping const& in, ChanMapping const& out,
	                     pframes_t nframes, samplecnt_t offset);

	bool has_editor () const;
	Steinberg::IPlugView* view ();
#if SMTG_OS_LINUX
	void set_runloop (Steinberg::Linux::IRunLoop*);
#endif

	bool configure_io (ChanCount in, ChanCount out);

	PBD::Signal2<void, int, int> OnResizeView;

private:
	samplecnt_t plugin_latency () const;
	void        init ();
	void        find_presets ();
	void        forward_resize_view (int w, int h);

	Steinberg::VST3PI* _plug;

	PBD::ScopedConnectionList _connections;
};

/* ****************************************************************************/

class LIBARDOUR_API VST3PluginInfo : public PluginInfo
{
public:
	VST3PluginInfo ();
	~VST3PluginInfo (){};

	PluginPtr                         load (Session& session);
	std::vector<Plugin::PresetRecord> get_presets (bool user_only) const;

	boost::shared_ptr<VST3PluginModule> m;
};

} // namespace ARDOUR
#endif
