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

#include "pbd/compose.h"
#include "pbd/error.h"
#include "pbd/failed_constructor.h"

#include "ardour/audio_buffer.h"
#include "ardour/audioengine.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/vst3_module.h"
#include "ardour/vst3_plugin.h"

#include "pbd/i18n.h"

using namespace PBD;
using namespace ARDOUR;
using namespace Steinberg;

VST3Plugin::VST3Plugin (AudioEngine& engine, Session& session, VST3PI* plug)
	: Plugin (engine, session)
	, _plug (plug)
{
	init ();
}

VST3Plugin::VST3Plugin (const VST3Plugin& other)
	: Plugin (other)
{
	boost::shared_ptr<VST3PluginInfo> nfo = boost::dynamic_pointer_cast<VST3PluginInfo> (other.get_info ());
	_plug = new VST3PI (nfo->m, nfo->index, nfo->unique_id);
	init ();
}

VST3Plugin::~VST3Plugin ()
{
	delete _plug;
}

void
VST3Plugin::init ()
{
	Vst::ProcessContext& context (_plug->context ());
	context.sampleRate = _session.nominal_sample_rate ();
	_plug->set_block_size (_session.get_block_size ());
	_plug->OnResizeView.connect_same_thread (_connections, boost::bind (&VST3Plugin::forward_resize_view, this, _1, _2));
}

void
VST3Plugin::forward_resize_view (int w, int h) {
	OnResizeView (w, h); /* EMIT SINAL */
}

uint32_t
VST3Plugin::parameter_count () const
{
	return _plug->parameter_count ();
}

float
VST3Plugin::default_value (uint32_t port)
{
	assert (port < parameter_count ());
	return _plug->default_value (port);
}

void
VST3Plugin::set_parameter (uint32_t port, float val, sampleoffset_t when)
{
	_plug->set_parameter (port, val, when);
}

float
VST3Plugin::get_parameter (uint32_t port) const
{
	return _plug->get_parameter (port);
}

int
VST3Plugin::get_parameter_descriptor (uint32_t port, ParameterDescriptor& desc) const
{
	assert (port < parameter_count ());
	_plug->get_parameter_descriptor (port, desc);
	desc.update_steps ();
	return 0;
}

uint32_t
VST3Plugin::nth_parameter (uint32_t port, bool& ok) const
{
	if (port < parameter_count ()) {
		ok = true;
		return port;
	}
	ok = false;
	return 0;
}

bool
VST3Plugin::parameter_is_input (uint32_t port) const
{
	return !_plug->parameter_is_readonly (port);
}

bool
VST3Plugin::parameter_is_output (uint32_t port) const
{
	return _plug->parameter_is_readonly (port);
}

uint32_t
VST3Plugin::designated_bypass_port ()
{
	return _plug->designated_bypass_port ();
}

std::set<Evoral::Parameter>
VST3Plugin::automatable () const
{
	std::set<Evoral::Parameter> automatables;
	for (uint32_t i = 0; i < parameter_count (); ++i) {
		if (parameter_is_input (i) && _plug->parameter_is_automatable (i)) {
			automatables.insert (automatables.end (), Evoral::Parameter (PluginAutomation, 0, i));
		}
	}
	return automatables;
}

std::string
VST3Plugin::describe_parameter (Evoral::Parameter param)
{
	if (param.type () == PluginAutomation && param.id () < parameter_count ()) {
		return _plug->parameter_label (param.id ());
	}
	return "??";
}

bool
VST3Plugin::print_parameter (uint32_t port, std::string& rv) const
{
	rv = _plug->print_parameter (port);
	return rv.size() > 0;
}

Plugin::IOPortDescription
VST3Plugin::describe_io_port (ARDOUR::DataType dt, bool input, uint32_t id) const
{
	return Plugin::describe_io_port (dt, input, id); // TODO
}

PluginOutputConfiguration
VST3Plugin::possible_output () const
{
	return Plugin::possible_output (); // TODO
}

bool
VST3Plugin::has_editor () const
{
	IPlugView* view = const_cast<VST3PI*>(_plug)->view ();
	if (view){
#ifdef PLATFORM_WINDOWS
		return kResultOk == view->isPlatformTypeSupported ("HWND");
#elif defined (__APPLE__)
		return kResultOk == view->isPlatformTypeSupported ("NSView");
#else
		return kResultOk == view->isPlatformTypeSupported ("X11EmbedWindowID");
#endif
	}
	return false;
}

Steinberg::IPlugView*
VST3Plugin::view ()
{
	return _plug->view ();
}

#if SMTG_OS_LINUX
void
VST3Plugin::set_runloop (Steinberg::Linux::IRunLoop* run_loop)
{
	return _plug->set_runloop (run_loop);
}
#endif

bool
VST3PI::evoral_to_vst3 (Vst::Event& e, Evoral::Event<samplepos_t> const& ev, int32_t bus)
{
	const uint32_t size = ev.size ();
	if (size == 0) {
		return false;
	}

	const uint8_t* data   = ev.buffer();
	uint8_t        status = data[0];

	if (status >= 0x80 && status < 0xF0) {
		status &= 0xf0;
	}

	if (size == 2 || size == 3) {
		Vst::ParamID id = Vst::kNoParamId;

		const uint8_t  channel = data[0] & 0x0f;
		const uint8_t  data1   = data[1] & 0x7f;
		const uint8_t  data2   = size == 3 ? (data[2] & 0x7f) : 0;

		switch (status) {
			case MIDI_CMD_NOTE_OFF:
				e.type = Vst::Event::kNoteOffEvent;
				e.noteOff.channel  = channel;
				e.noteOff.noteId   = -1;
				e.noteOff.pitch    = data1;
				e.noteOff.velocity = data2 / 127.f;
				e.noteOff.tuning   = 0.f;
				return true;
			case MIDI_CMD_NOTE_ON:
				e.type = Vst::Event::kNoteOnEvent;
				e.noteOn.channel  = channel;
				e.noteOn.noteId   = -1;
				e.noteOn.pitch    = data1;
				e.noteOn.velocity = data2 / 127.f;
				e.noteOn.length   = 0;
				e.noteOn.tuning   = 0.f;
				return true;
			case MIDI_CMD_NOTE_PRESSURE:
				e.type = Vst::Event::kPolyPressureEvent;
				e.polyPressure.channel  = channel;
				e.polyPressure.pitch    = data1;
				e.polyPressure.pressure = data2 / 127.f;
				e.polyPressure.noteId   = -1;
				return true;
			case MIDI_CMD_CONTROL:
				if (midi_controller (bus, channel, data1, id)) {
					set_parameter (id, data2 / 127.f, ev.time ());
				}
				return false;
			case MIDI_CMD_PGM_CHANGE:
				assert (size == 2);
				if (_port_id_program_change != UINT32_MAX) {
					set_parameter (_port_id_program_change, data1 / 127.f, ev.time ());
				}
				return false;
			case MIDI_CMD_CHANNEL_PRESSURE:
				assert (size == 2);
				if (midi_controller (bus, channel, Vst::kAfterTouch, id)) {
					set_parameter (id, data1 / 127.f, ev.time ());
				}
				return false;
			case MIDI_CMD_BENDER:
				if (midi_controller (bus, channel, Vst::kPitchBend, id)) {
					uint32_t m14 = (data2 << 7) | data1;
					set_parameter (id, m14 / 127.f, ev.time ());
				}
				return false;
		}
	} else if (status == MIDI_CMD_COMMON_SYSEX) {
		memset (&e, 0, sizeof(Vst::Event));
		e.type       = Vst::Event::kDataEvent;
		e.data.type  = Vst::DataEvent::kMidiSysEx;
		e.data.bytes = ev.buffer (); // TODO copy ?!
		e.data.size  = ev.size();
		return true;
	}
	return false;
}

/* ****************************************************************************/

void
VST3Plugin::add_state (XMLNode* root) const
{
	XMLNode *child;
	for (uint32_t i = 0; i < parameter_count (); ++i) {
		if (!_plug->parameter_is_automatable (i)) {
			continue;
		}
		child = new XMLNode("Port");
		child->set_property("id", (uint32_t) _plug->index_to_id(i));
		child->set_property("value", _plug->get_parameter (i));
		root->add_child_nocopy (*child);
	}
}

int
VST3Plugin::set_state (const XMLNode& node, int version)
{
	XMLNodeConstIterator iter;

	if (node.name() != state_node_name()) {
		error << string_compose (_("VST3<%1>: Bad node sent to VST3Plugin::set_state"), name ()) << endmsg;
		return -1;
	}

	XMLNodeList nodes = node.children ("Port");
	for (iter = nodes.begin(); iter != nodes.end(); ++iter) {
		XMLNode* child = *iter;

		uint32_t param_id;
		float value;

		if (!child->get_property ("id", param_id)) {
			warning << string_compose (_("VST3<%1>: Missing parameter-id in VST3Plugin::set_state"), name ()) << endmsg;
			continue;
		}

		if (!child->get_property ("value", value)) {
			warning << string_compose (_("VST3<%1>: Missing parameter value in VST3Plugin::set_state"), name ()) << endmsg;
			continue;
		}

		if (!_plug->set_parameter_by_id ( param_id, value)) {
			warning << string_compose (_("VST3<%1>: Invalid Vst::ParamID in VST3Plugin::set_state"), name ()) << endmsg;
		}
	}

	return Plugin::set_state (node, version);
}

/* ****************************************************************************/

int
VST3Plugin::set_block_size (pframes_t n_samples)
{
	_plug->set_block_size (n_samples);
	return 0;
}

samplecnt_t
VST3Plugin::plugin_latency () const
{
	return _plug->plugin_latency ();
}

bool
VST3Plugin::configure_io (ChanCount in, ChanCount out)
{
	return Plugin::configure_io (in, out);
}

int
VST3Plugin::connect_and_run (BufferSet&  bufs,
                             samplepos_t start, samplepos_t end, double speed,
                             ChanMapping const& in_map, ChanMapping const& out_map,
                             pframes_t n_samples, samplecnt_t offset)
{
	//DEBUG_TRACE(DEBUG::VST3, string_compose("%1 run %2 offset %3\n", name(), nframes, offset));
	Plugin::connect_and_run (bufs, start, end, speed, in_map, out_map, n_samples, offset);

	Vst::ProcessContext& context (_plug->context ());

	/* clear event ports */
	_plug->cycle_start();

	context.state =
	    Vst::ProcessContext::kContTimeValid | Vst::ProcessContext::kSystemTimeValid | Vst::ProcessContext::kSmpteValid | Vst::ProcessContext::kProjectTimeMusicValid | Vst::ProcessContext::kBarPositionValid | Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid | Vst::ProcessContext::kClockValid;

	context.projectTimeSamples   = start;
	context.continousTimeSamples = _session.engine ().processed_samples ();
	context.systemTime           = g_get_monotonic_time ();

	{
		TempoMap const&     tmap (_session.tempo_map ());
		const Tempo&        t (tmap.tempo_at_sample (start));
		const MeterSection& ms (tmap.meter_section_at_sample (start));
		context.tempo              = t.quarter_notes_per_minute ();
		context.timeSigNumerator   = ms.divisions_per_bar ();
		context.timeSigDenominator = ms.note_divisor ();
	}

	const double tcfps                = _session.timecode_frames_per_second ();
	context.frameRate.framesPerSecond = ceil (tcfps);
	context.frameRate.flags           = 0;
	if (_session.timecode_drop_frames ()) {
		context.frameRate.flags = Vst::FrameRate::kDropRate; /* 29.97 */
	} else if (tcfps > context.frameRate.framesPerSecond) {
		context.frameRate.flags = Vst::FrameRate::kPullDownRate; /* 23.976 etc */
	}

	if (_session.get_play_loop ()) {
		Location* looploc = _session.locations ()->auto_loop_location ();
		try {
			/* loop start/end in quarter notes */
			TempoMap const& tmap (_session.tempo_map ());
			context.cycleStartMusic = tmap.quarter_note_at_sample_rt (looploc->start ());
			context.cycleEndMusic   = tmap.quarter_note_at_sample_rt (looploc->end ());
			context.state |= Vst::ProcessContext::kCycleValid;
		} catch (...) {
		}
	}
	if (speed != 0) {
		context.state |= Vst::ProcessContext::kPlaying;
	}
	if (_session.actively_recording ()) {
		context.state |= Vst::ProcessContext::kRecording;
	}
#if 0 // TODO
	context.state |= Vst::ProcessContext::kClockValid;
	context.samplesToNextClock = 0 // MIDI Clock Resolution (24 Per Quarter Note), can be negative (nearest);
#endif

	ChanCount bufs_count;
	bufs_count.set (DataType::AUDIO, 1);
	bufs_count.set (DataType::MIDI, 1);

	BufferSet& silent_bufs  = _session.get_silent_buffers (bufs_count);
	BufferSet& scratch_bufs = _session.get_scratch_buffers (bufs_count);

	float** ins  = (float**)alloca (_plug->n_audio_inputs () * sizeof (float*));
	float** outs = (float**)alloca (_plug->n_audio_outputs () * sizeof (float*));

	uint32_t in_index = 0;
	for (int32_t i = 0; i < (int32_t)_plug->n_audio_inputs (); ++i) {
		uint32_t index;
		bool     valid = false;
		index          = in_map.get (DataType::AUDIO, in_index++, &valid);
		ins[i]         = (valid)
		             ? bufs.get_audio (index).data (offset)
		             : silent_bufs.get_audio (0).data (offset);
	}

	uint32_t out_index = 0;
	for (int32_t i = 0; i < (int32_t)_plug->n_audio_outputs (); ++i) {
		uint32_t index;
		bool     valid = false;
		index          = out_map.get (DataType::AUDIO, out_index++, &valid);
		outs[i]        = (valid)
		              ? bufs.get_audio (index).data (offset)
		              : scratch_bufs.get_audio (0).data (offset);
	}

	// TODO MIDI
	in_index = 0;
	for (int32_t i = 0; i < (int32_t)_plug->n_midi_inputs (); ++i) {
		bool     valid = false;
		uint32_t index = in_map.get (DataType::MIDI, in_index++, &valid);
		if (valid && bufs.count().n_midi() > index) {
			for (MidiBuffer::iterator m = bufs.get_midi(index).begin(); m != bufs.get_midi(index).end(); ++m) {
				const Evoral::Event<samplepos_t> ev (*m, false);
				_plug->add_event (ev, i);
			}
		}
	}

	_plug->process (&ins[0], &outs[0], n_samples);

	{
		//_output_events.size ();
	}

	return 0;
}

/* ****************************************************************************/

bool
VST3Plugin::load_preset (PresetRecord r)
{
	return false;
}

std::string
VST3Plugin::do_save_preset (std::string name)
{
	return "";
}

void
VST3Plugin::do_remove_preset (std::string name)
{
}

void
VST3Plugin::find_presets ()
{
}

/* ****************************************************************************/

VST3PluginInfo::VST3PluginInfo ()
{
	type = ARDOUR::VST3;
}

PluginPtr
VST3PluginInfo::load (Session& session)
{
	try {
		if (!m) {
			printf ("Loading %s\n", path.c_str ());
			m = VST3PluginModule::load (path);
			printf ("Loaded module\n");
		}
		PluginPtr          plugin;
		Steinberg::VST3PI* plug = new VST3PI (m, index, unique_id);
		plugin.reset (new VST3Plugin (session.engine (), session, plug));
		plugin->set_info (PluginInfoPtr (new VST3PluginInfo (*this)));
		return plugin;
	} catch (failed_constructor& err) {
		;
	}

	return PluginPtr ();
}

std::vector<Plugin::PresetRecord>
VST3PluginInfo::get_presets (bool /*user_only*/) const
{
	std::vector<Plugin::PresetRecord> p;
	return p;
}

/* ****************************************************************************/

VST3PI::VST3PI (boost::shared_ptr<ARDOUR::VST3PluginModule> m, int index, std::string unique_id)
	: _module (m)
	, _factory (0)
	, _component (0)
	, _controller (0)
	, _view (0)
	, _is_processing (false)
	, _block_size (0)
	, _port_id_bypass (UINT32_MAX)
	, _port_id_program_change (UINT32_MAX)
{
	using namespace std;

	GetFactoryProc fp = (GetFactoryProc)m->fn_ptr ("GetPluginFactory");
	if (!fp) {
		throw failed_constructor ();
	}

	if (!(_factory = fp ())) {
		throw failed_constructor ();
	}

	FUID fuid;
	if (!fuid.fromString (unique_id.c_str ())) {
		throw failed_constructor ();
	}

	PClassInfo ci;
	if (_factory->getClassInfo (index, &ci) != kResultTrue) {
		throw failed_constructor ();
	}
	if (strcmp (ci.category, kVstAudioEffectClass)) {
		throw failed_constructor ();
	}
	if (FUID::fromTUID (ci.cid) != fuid) {
		throw failed_constructor ();
	}

	if (_factory->createInstance (ci.cid, Vst::IComponent::iid, (void**)&_component) != kResultTrue) {
		throw failed_constructor ();
	}

	if (_component->initialize (HostApplication::getHostContext ()) != kResultOk) {
		throw failed_constructor ();
	}

	_controller = FUnknownPtr<Vst::IEditController> (_component);
	if (!_controller) {
		TUID controllerCID;
		if (_component->getControllerClassId (controllerCID) == kResultTrue) {
			if (_factory->createInstance (controllerCID, Vst::IEditController::iid, (void**)&_controller) != kResultTrue) {
				throw failed_constructor ();
			}
			if (_controller && (_controller->initialize (HostApplication::getHostContext ()) != kResultOk)) {
				throw failed_constructor ();
			}
		}
	}

	if (!_controller) {
		_component->terminate ();
		throw failed_constructor ();
	}

	if (_controller->setComponentHandler (this) != kResultOk) {
		_component->terminate ();
		throw failed_constructor ();
	}

	if (!(_processor = FUnknownPtr<Vst::IAudioProcessor> (_component))) {
		//_controller->terminate();
		_component->terminate ();
		throw failed_constructor ();
	}

	/* prepare process context */
	memset (&_context, 0, sizeof (Vst::ProcessContext));

	_n_inputs       = count_channels (Vst::kAudio, Vst::kInput,  Vst::kMain);
	_n_aux_inputs   = count_channels (Vst::kAudio, Vst::kInput,  Vst::kAux);
	_n_outputs      = count_channels (Vst::kAudio, Vst::kOutput, Vst::kMain);
	_n_aux_outputs  = count_channels (Vst::kAudio, Vst::kOutput, Vst::kAux);
	_n_midi_inputs  = count_channels (Vst::kEvent, Vst::kInput,  Vst::kMain);
	_n_midi_outputs = count_channels (Vst::kEvent, Vst::kOutput, Vst::kMain);

	if (!connect_components ()) {
		//_controller->terminate();
		_component->terminate ();
		throw failed_constructor ();
	}

#if 0
	WriteStream stream;
	if (_component->getState (&stream) == kResultTrue) {
		stream.rewind();
		if (_controller->setComponentState (&stream) != kResultTrue){
			cerr << "Failed to synchronize VST3 component <> controller state\n";
		}
	}
#endif

	int32 n_params = _controller->getParameterCount ();
	for (int32 i = 0; i < n_params; ++i) {
		Vst::ParameterInfo pi;
		if (_controller->getParameterInfo (i, pi) != kResultTrue) {
			continue;
		}
		if (pi.flags & Vst::ParameterInfo::kIsProgramChange) {
			_port_id_program_change = pi.id;
			continue;
		}
		if (0 == (pi.flags & Vst::ParameterInfo::kCanAutomate)) {
			/* but allow read-only, not automatable params (ctrl outputs) */
			if (0 == (pi.flags & Vst::ParameterInfo::kIsReadOnly)) {
				continue;
			}
		}
		if (tchar_to_utf8 (pi.title).find("MIDI CC ") != std::string::npos) {
			/* Some JUCE plugins add 16 * 128 automatable MIDI CC parameters */
			continue;
		}

		Param p;
		p.id        = pi.id;
		p.label     = tchar_to_utf8 (pi.title).c_str ();
		p.unit      = tchar_to_utf8 (pi.units).c_str ();
		p.steps     = pi.stepCount;
		p.is_enum   = 0 != (pi.flags & Vst::ParameterInfo::kIsList);
		p.read_only = 0 != (pi.flags & Vst::ParameterInfo::kIsReadOnly);
		p.normal    = pi.defaultNormalizedValue;

		uint32_t idx = _ctrl_params.size ();
		_ctrl_params.push_back (p);

		if (pi.flags & Vst::ParameterInfo::kIsBypass) {
			_port_id_bypass = idx;
		}
		_ctrl_id_index[pi.id] = idx;
		_ctrl_index_id[idx]   = pi.id;

		_shadow_data.push_back (p.normal);
	}

	_input_param_changes.set_n_params (n_params);
	_output_param_changes.set_n_params (n_params);
}

VST3PI::~VST3PI ()
{
	_processor = 0;
	terminate ();
}

void
VST3PI::terminate ()
{
	disconnect_components ();

	bool controller_is_component = false;
	if (_component) {
		controller_is_component = FUnknownPtr<Vst::IEditController> (_component) != 0;
		_component->terminate ();
	}

	if (_controller && controller_is_component == false) {
		_controller->terminate ();
	}
	_component  = 0;
	_controller = 0;
}

bool
VST3PI::connect_components ()
{
	if (!_component || !_controller) {
		return false;
	}

	FUnknownPtr<Vst::IConnectionPoint> componentCP (_component);
	FUnknownPtr<Vst::IConnectionPoint> controllerCP (_controller);

	if (!componentCP || !controllerCP) {
		return true;
	}

	if (componentCP->connect (this) == kResultTrue && controllerCP->connect (this) == kResultTrue) {
		return true;
	}
	return false;
}

bool
VST3PI::disconnect_components ()
{
	FUnknownPtr<Vst::IConnectionPoint> componentCP (_component);
	FUnknownPtr<Vst::IConnectionPoint> controllerCP (_controller);
	if (!componentCP || !controllerCP) {
		return false;
	}

	bool res = componentCP->disconnect (this);
	res &= controllerCP->disconnect (this);
	return res;
}

tresult
VST3PI::connect (Vst::IConnectionPoint* other)
{
	return other ? kResultTrue : kInvalidArgument;
}

tresult
VST3PI::disconnect (Vst::IConnectionPoint* other)
{
	return kResultTrue;
}

tresult
VST3PI::notify (Vst::IMessage* msg)
{
	printf ("VST3PI::notify\n");
	FUnknownPtr<Vst::IConnectionPoint> componentCP (_component);
	FUnknownPtr<Vst::IConnectionPoint> controllerCP (_controller);
	// XXX this bounces the message back..
	// we likely need a proxy here
	if (componentCP) {
		componentCP->notify (msg);
	}
	if (controllerCP) {
		controllerCP->notify (msg);
	}
	return kResultTrue;
}

tresult
VST3PI::queryInterface (const TUID _iid, void** obj)
{
	QUERY_INTERFACE (_iid, obj, FUnknown::iid, Vst::IComponentHandler)
	QUERY_INTERFACE (_iid, obj, Vst::IComponentHandler::iid, Vst::IComponentHandler)

#if SMTG_OS_LINUX
	if (_run_loop && FUnknownPrivate::iidEqual (_iid, Linux::IRunLoop::iid)) {
		*obj = _run_loop;
		return kResultOk;
	}
#endif

	*obj = nullptr;
	return kNoInterface;
}

tresult
VST3PI::restartComponent (int32 flags)
{
	printf ("VST3PI::restartComponent %d", flags);
	// might indicate latency change  kLatencyChanged
	return kResultOk;
}

tresult
VST3PI::performEdit (Vst::ParamID id, Vst::ParamValue value)
{
	return kResultOk;
}

tresult
VST3PI::beginEdit (Vst::ParamID id)
{
	return kResultOk;
}

tresult
VST3PI::endEdit (Vst::ParamID id)
{
	return kResultOk;
}

bool
VST3PI::deactivate ()
{
	if (!_is_processing) {
		return true;
	}
	if (_processor->setProcessing (false) != kResultOk) {
		return false;
	}
	if (_component->setActive (false) != kResultOk) {
		return false;
	}
	_is_processing = false;
	return true;
}

bool
VST3PI::activate ()
{
	if (_is_processing) {
		return true;
	}

	if (_component->setActive (true) != kResultOk) {
		return false;
	}

	if (_processor->setProcessing (true) != kResultOk) {
		return false;
	}
	_is_processing = true;
	return true;
}

bool
VST3PI::update_processor ()
{
	bool was_active = _is_processing;

	if (!deactivate ()) {
		return false;
	}

	Vst::ProcessSetup setup;
	setup.processMode        = Vst::kRealtime;
	setup.symbolicSampleSize = Vst::kSample32;
	setup.maxSamplesPerBlock = _block_size;
	setup.sampleRate         = _context.sampleRate;

	if (_processor->setupProcessing (setup) != kResultOk) {
		return false;
	}

	if (was_active) {
		return activate ();
	}
	return true;
}

int32
VST3PI::count_channels (Vst::MediaType media, Vst::BusDirection dir, Vst::BusType type) const
{
	int32 n_busses = _component->getBusCount (media, dir);
	for (int32 i = 0; i < n_busses; ++i) {
		Vst::BusInfo bus;
		if (_component->getBusInfo (media, dir, i, bus) == kResultTrue && bus.busType == type) {
			if ((type == Vst::kMain && i != 0) || (type == Vst::kAux && i != 1)) {
				std::cerr << "Invaldid bus index\n";
				return 0;
			}
			return bus.channelCount;
		}
	}
	return 0;
}

Vst::ParamID
VST3PI::index_to_id (uint32_t p) const
{
	assert (_ctrl_index_id.find (p) != _ctrl_index_id.end());
	return (_ctrl_index_id.find (p))->second;
}

bool
VST3PI::set_block_size (int32_t n_samples)
{
	if (_block_size == n_samples) {
		return true;
	}
	_block_size = n_samples;
	return update_processor ();
}

float
VST3PI::default_value (uint32_t port) const
{
	Vst::ParamID id (index_to_id (port));
	return _controller->normalizedParamToPlain (id, _ctrl_params[port].normal);
}

void
VST3PI::get_parameter_descriptor (uint32_t port, ParameterDescriptor& desc) const
{
	Param const& p  (_ctrl_params[port]);
	Vst::ParamID id (index_to_id (port));

	desc.lower        = _controller->normalizedParamToPlain (id, 0.f);
	desc.upper        = _controller->normalizedParamToPlain (id, 1.f);
	desc.normal       = _controller->normalizedParamToPlain (id, p.normal);
	desc.toggled      = 1 == p.steps;
	desc.logarithmic  = false;
	desc.integer_step = p.steps > 1 ? p.steps : 0;
	desc.sr_dependent = false;
	desc.enumeration  = p.is_enum;
	desc.label        = p.label;
	if (p.unit == "dB") {
		desc.unit = ARDOUR::ParameterDescriptor::DB;
	} else if (p.unit == "Hz") {
		desc.unit = ARDOUR::ParameterDescriptor::HZ;
	}
}

std::string
VST3PI::print_parameter (uint32_t port) const
{
	Vst::ParamID id (index_to_id (port));
	Vst::String128 rv;
	if (_controller->getParamStringByValue (id, _shadow_data[port], rv) == kResultOk) {
		return tchar_to_utf8 (rv);
	}
	return "";
}

uint32_t
VST3PI::n_audio_inputs () const
{
	return _n_inputs + _n_aux_inputs;
}

uint32_t
VST3PI::n_audio_outputs () const
{
	return _n_outputs + _n_aux_outputs;
}

uint32_t
VST3PI::n_midi_inputs () const
{
	return _n_midi_inputs;
}

uint32_t
VST3PI::n_midi_outputs () const
{
	return _n_midi_outputs;
}

bool
VST3PI::set_parameter_by_id (Vst::ParamID id, float value)
{
	std::map<Vst::ParamID, uint32_t>::const_iterator idx = _ctrl_id_index.find (id);
	if (idx == _ctrl_id_index.end()) {
		return false;
	}
	set_parameter (idx->second, value, 0);
	return true;
}

void
VST3PI::set_parameter (uint32_t p, float value, int32 sample_off)
{
	Vst::ParamID id (index_to_id (p));
	int32 unused;
	value = _controller->plainParamToNormalized (id, value);
	_input_param_changes.addParameterData (id, unused)->addPoint (sample_off, value, unused);
	_shadow_data[p] = value;
	_controller->setParamNormalized (id, value);
}

float
VST3PI::get_parameter (uint32_t p) const
{
	Vst::ParamID id = index_to_id (p);
	return _controller->normalizedParamToPlain (id, _shadow_data[p]);
}

bool
VST3PI::midi_controller (int32_t bus, int16_t channel, Vst::CtrlNumber ctrl, Vst::ParamID &id)
{
	FUnknownPtr<Vst::IMidiMapping> midiMapping (_controller);
	if (!midiMapping) {
		return false;
	}
	return kResultOk == midiMapping->getMidiControllerAssignment (bus, channel, ctrl, id);
}

IPlugView*
VST3PI::view ()
{
	if (!_view) {
		_view = _controller->createView ("editor");
		if (_view) {
			_view->setFrame (this);
		}
	}
	return _view;
}

#if SMTG_OS_LINUX
void
VST3PI::set_runloop (Linux::IRunLoop* run_loop)
{
	_run_loop = run_loop;
}
#endif

tresult
VST3PI::resizeView (IPlugView* view, ViewRect* new_size)
{
	OnResizeView (new_size->getWidth (), new_size->getHeight ()); /* EMIT SIGNAL */
	return view->onSize (new_size);
}

void
VST3PI::cycle_start ()
{
	_input_events.clear ();
	_output_events.clear ();
}

void
VST3PI::add_event (Evoral::Event<samplepos_t> const& ev, int32_t bus)
{
	Vst::Event e;
	e.busIndex     = bus;
	e.flags        = Vst::Event::kIsLive;
	e.sampleOffset = ev.time();
	e.ppqPosition  = _context.projectTimeMusic;
	if (evoral_to_vst3 (e, ev, bus)) {
		_input_events.addEvent (e);
	}
}

void
VST3PI::process (float** ins, float** outs, uint32_t n_samples)
{
	Vst::AudioBusBuffers input[2]; // in-bus & aux-bus
	Vst::AudioBusBuffers output[2];

	Vst::ProcessData data;
	data.numSamples         = n_samples;
	data.symbolicSampleSize = Vst::kSample32;
	data.numInputs          = _n_aux_inputs > 0 ? 2 : 1;
	data.numOutputs         = _n_aux_outputs > 0 ? 2 : 1;
	data.inputs             = input;
	data.outputs            = output;

	data.processContext = &_context;
	data.inputEvents = &_input_events;
	data.outputEvents = &_output_events;

	data.inputParameterChanges  = &_input_param_changes;
	data.outputParameterChanges = &_output_param_changes;

	input[0].silenceFlags     = 0;
	input[0].numChannels      = _n_inputs;
	input[0].channelBuffers32 = ins;

	if (_n_aux_inputs > 0) {
		input[1].silenceFlags     = 0;
		input[1].numChannels      = _n_aux_inputs;
		input[1].channelBuffers32 = &ins[_n_inputs + 1];
	}

	output[0].silenceFlags     = 0;
	output[0].numChannels      = _n_outputs;
	output[0].channelBuffers32 = outs;

	if (_n_aux_outputs > 0) {
		output[1].silenceFlags     = 0;
		output[1].numChannels      = _n_outputs;
		output[1].channelBuffers32 = &outs[_n_outputs + 1];
	}

	/* and go */
	if (_processor->process (data) != kResultOk) {
		printf ("VST3: Process error\n"); // XXX
	}

	/* handle output parameter changes */
	int n_changes = _output_param_changes.getParameterCount();
	for (int i = 0; i < n_changes; ++i) {
		Vst::IParamValueQueue* data = _output_param_changes.getParameterData (i);
		if (!data) {
			continue;
		}
		Vst::ParamID id = data->getParameterId();
		int n_points = data->getPointCount();

		if (n_points == 0) {
			continue;
		}

		std::map<Vst::ParamID, uint32_t>::const_iterator idx = _ctrl_id_index.find (id);
		if (idx != _ctrl_id_index.end()) {
			/* automatable parameter, or read-only output */
			int32 offset = 0;
			Vst::ParamValue value = 0;
			/* only get most recent point */
			if (data->getPoint(n_points - 1, offset, value) == kResultOk) {
				_shadow_data[idx->second] = (float) value;
			}
		} else {
			/* non-automatable parameter */
			std::cerr << "VST3: TODO non-automatable output param..\n"; // TODO inform UI
		}
	}

	_input_param_changes.clear ();
	_output_param_changes.clear ();
}
