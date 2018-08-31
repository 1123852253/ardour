/*
    Copyright (C) 2008 Paul Davis
    Author: Hans Baier

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

*/

#include <cmath>
#include <errno.h>
#include <sys/types.h>
#include <unistd.h>
#include "pbd/error.h"
#include "pbd/failed_constructor.h"
#include "pbd/pthread_utils.h"
#include "pbd/convert.h"

#include "midi++/port.h"

#include "ardour/audioengine.h"
#include "ardour/debug.h"
#include "ardour/midi_buffer.h"
#include "ardour/midi_port.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/transport_master.h"

#include "pbd/i18n.h"

using namespace std;
using namespace ARDOUR;
using namespace MIDI;
using namespace PBD;

#define ENGINE AudioEngine::instance()

MIDIClock_TransportMaster::MIDIClock_TransportMaster (std::string const & name, int ppqn)
	: TransportMaster (MIDIClock, name)
	, ppqn (ppqn)
	, last_timestamp (0)
	, should_be_position (0)
	, midi_clock_count (0)
	, _speed (0)
	, _running (false)
	, _bpm (0)
{
	if ((_port = create_midi_port (string_compose ("%1 in", name))) == 0) {
		throw failed_constructor();
	}
}

MIDIClock_TransportMaster::~MIDIClock_TransportMaster()
{
	port_connections.drop_connections ();
}

void
MIDIClock_TransportMaster::set_session (Session *session)
{
	port_connections.drop_connections();
	_session = session;

	/* only connect to signals if we have a proxy, because otherwise we
	 * cannot interpet incoming data (no tempo map etc.)
	 */

	if (_session) {
		parser.timing.connect_same_thread (port_connections, boost::bind (&MIDIClock_TransportMaster::update_midi_clock, this, _1, _2));
		parser.start.connect_same_thread (port_connections, boost::bind (&MIDIClock_TransportMaster::start, this, _1, _2));
		parser.contineu.connect_same_thread (port_connections, boost::bind (&MIDIClock_TransportMaster::contineu, this, _1, _2));
		parser.stop.connect_same_thread (port_connections, boost::bind (&MIDIClock_TransportMaster::stop, this, _1, _2));
		parser.position.connect_same_thread (port_connections, boost::bind (&MIDIClock_TransportMaster::position, this, _1, _2, 3));

		reset ();
	}
}

bool
MIDIClock_TransportMaster::speed_and_position (double& speed, samplepos_t& pos, samplepos_t now)
{
	if (!_running) {
		speed = 0.0;
		pos   = should_be_position;
		_current_delta = 0;
		return true;
	}

	if (fabs (_speed - 1.0) < 0.001) {
		speed = 1.0;
	} else {
		speed = _speed;
	}

	pos = should_be_position;
	pos += (now - last_timestamp) * _speed;

	return true;
}

void
MIDIClock_TransportMaster::pre_process (pframes_t nframes, samplepos_t now, boost::optional<samplepos_t> session_pos)
{
	/* Read and parse incoming MIDI */

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("preprocess with lt = %1 @ %2, running ? %3\n", last_timestamp, now, _running));

	update_from_midi (nframes, now);

	/* no timecode ever, or no timecode for 1/4 second ? conclude that its stopped */

	if (!last_timestamp || (now > last_timestamp && now - last_timestamp > ENGINE->sample_rate() / 4)) {
		_speed = 0.0;
		_bpm = 0.0;
		last_timestamp = 0;
		_running = false;

		DEBUG_TRACE (DEBUG::MidiClock, "No MIDI Clock messages received for some time, stopping!\n");
		return;
	}

	if (!_running) {
		if (session_pos) {
			should_be_position = *session_pos;
		}
	}

	if (session_pos) {
		const samplepos_t current_pos = should_be_position + ((now - last_timestamp) * _speed);
		_current_delta = current_pos - *session_pos;
	} else {
		_current_delta = 0;
	}

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("speed_and_position: speed %1 should-be %2 transport %3 \n", _speed, should_be_position, _session->transport_sample()));
}

void
MIDIClock_TransportMaster::calculate_one_ppqn_in_samples_at(samplepos_t time)
{
	const double samples_per_quarter_note = _session->tempo_map().samples_per_quarter_note_at (time, ENGINE->sample_rate());

	one_ppqn_in_samples = samples_per_quarter_note / double (ppqn);
	// DEBUG_TRACE (DEBUG::MidiClock, string_compose ("at %1, one ppqn = %2\n", time, one_ppqn_in_samples));
}

ARDOUR::samplepos_t
MIDIClock_TransportMaster::calculate_song_position(uint16_t song_position_in_sixteenth_notes)
{
	samplepos_t song_position_samples = 0;
	for (uint16_t i = 1; i <= song_position_in_sixteenth_notes; ++i) {
		// one quarter note contains ppqn pulses, so a sixteenth note is ppqn / 4 pulses
		calculate_one_ppqn_in_samples_at(song_position_samples);
		song_position_samples += one_ppqn_in_samples * (samplepos_t)(ppqn / 4);
	}

	return song_position_samples;
}

void
MIDIClock_TransportMaster::calculate_filter_coefficients()
{
	const double  bandwidth = (2.0 / 60.0); // 1 BpM = 1 / 60 Hz
	// omega = 2 * PI * Bandwidth / MIDI clock sample frequency in Hz
	const double omega = 2.0 * M_PI * bandwidth * one_ppqn_in_samples / ENGINE->sample_rate();
	b = 1.4142135623730950488 * omega;
	c = omega * omega;
}

void
MIDIClock_TransportMaster::update_midi_clock (Parser& /*parser*/, samplepos_t timestamp)
{
	calculate_one_ppqn_in_samples_at (should_be_position);

	samplepos_t elapsed_since_start = timestamp - first_timestamp;
	double error = 0;

	if (last_timestamp == 0) {
		midi_clock_count = 0;

		first_timestamp = timestamp;
		elapsed_since_start = should_be_position;

		DEBUG_TRACE (DEBUG::MidiClock, string_compose ("first clock message after start received @ %1\n", timestamp));

		// calculate filter coefficients
		calculate_filter_coefficients();

		// initialize DLL
		e2 = double(one_ppqn_in_samples) / double(ENGINE->sample_rate());
		t0 = double(elapsed_since_start) / double(ENGINE->sample_rate());
		t1 = t0 + e2;

	} else {
		midi_clock_count++;
		should_be_position += one_ppqn_in_samples;
		calculate_filter_coefficients();

		error = should_be_position - _session->audible_sample();
		const double e = error / ENGINE->sample_rate();
		_current_delta = error;

		// update DLL
		t0 = t1;
		t1 += b * e + e2;
		e2 += c * e;

		const double predicted_clock_interval_in_samples = (t1 - t0) * ENGINE->sample_rate();
		const double predicted_quarter_interval_in_samples = predicted_clock_interval_in_samples * 24.0;
		_speed = predicted_clock_interval_in_samples / one_ppqn_in_samples;
		_bpm = (ENGINE->sample_rate() * 60.0) / predicted_quarter_interval_in_samples;

		cerr << "apparent BPM = " << _bpm << " clock interval was " << predicted_clock_interval_in_samples << " p-quarter = " << predicted_quarter_interval_in_samples << endl;

		// need at least two clock events to compute speed

		if (!_running) {
			DEBUG_TRACE (DEBUG::MidiClock, string_compose ("start mclock running with speed = %1\n", ((t1 - t0) * ENGINE->sample_rate()) / one_ppqn_in_samples));
			_running = true;
		}
	}

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("clock #%1 @ %2 should-be %3 transport %4 error %5 appspeed %6 "
						       "read-delta %7 should-be delta %8 t1-t0 %9 t0 %10 t1 %11 framerate %12 engine %13 running %14\n",
						       midi_clock_count,                                          // #
						       elapsed_since_start,                                       // @
						       should_be_position,                                        // should-be
						       _session->transport_sample(),                                // transport
						       error,                                                     // error
						       ((t1 - t0) * ENGINE->sample_rate()) / one_ppqn_in_samples, // appspeed
						       timestamp - last_timestamp,                                // read delta
						       one_ppqn_in_samples,                                        // should-be delta
						       (t1 - t0) * ENGINE->sample_rate(),                         // t1-t0
						       t0 * ENGINE->sample_rate(),                                // t0
						       t1 * ENGINE->sample_rate(),                                // t1
						       ENGINE->sample_rate(),                                      // framerate
	                                               ENGINE->sample_time(),
	                                               _running

	));

	last_timestamp = timestamp;
}

void
MIDIClock_TransportMaster::start (Parser& /*parser*/, samplepos_t timestamp)
{
	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("MIDIClock_TransportMaster got start message at time %1 engine time %2 transport_sample %3\n", timestamp, ENGINE->sample_time(), _session->transport_sample()));

	if (!_running) {
		reset();
		_running = true;
		should_be_position = _session->transport_sample();
	}
}

void
MIDIClock_TransportMaster::reset ()
{
	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("MidiClock Master reset(): calculated filter for period size %2\n", ENGINE->samples_per_cycle()));

	should_be_position = _session->transport_sample();
	_speed = 0;
	last_timestamp = 0;

	_running = false;
	_current_delta = 0;
}

void
MIDIClock_TransportMaster::contineu (Parser& /*parser*/, samplepos_t /*timestamp*/)
{
	DEBUG_TRACE (DEBUG::MidiClock, "MIDIClock_TransportMaster got continue message\n");

	_running = true;
}

void
MIDIClock_TransportMaster::stop (Parser& /*parser*/, samplepos_t /*timestamp*/)
{
	DEBUG_TRACE (DEBUG::MidiClock, "MIDIClock_TransportMaster got stop message\n");

	if (_running) {
		_running = false;
		_speed = 0;
		last_timestamp = 0;

		// we need to go back to the last MIDI beat (6 ppqn)
		// and lets hope the tempo didnt change in the meantime :)

		// begin at the should be position, because
		// that is the position of the last MIDI Clock
		// message and that is probably what the master
		// expects where we are right now
		//
		// find out the last MIDI beat: go back #midi_clocks mod 6
		// and lets hope the tempo didnt change in those last 6 beats :)
		should_be_position -= (midi_clock_count % 6) * one_ppqn_in_samples;
	}
}

void
MIDIClock_TransportMaster::position (Parser& /*parser*/, MIDI::byte* message, size_t size)
{
	// we are not supposed to get position messages while we are running
	// so lets be robust and ignore those
	if (_running) {
		return;
	}

	assert(size == 3);
	MIDI::byte lsb = message[1];
	MIDI::byte msb = message[2];
	assert((lsb <= 0x7f) && (msb <= 0x7f));

	uint16_t position_in_sixteenth_notes = (uint16_t(msb) << 7) | uint16_t(lsb);
	samplepos_t position_in_samples = calculate_song_position(position_in_sixteenth_notes);

	DEBUG_TRACE (DEBUG::MidiClock, string_compose ("Song Position: %1 samples: %2\n", position_in_sixteenth_notes, position_in_samples));

	should_be_position  = position_in_samples;
	last_timestamp = 0;

}

bool
MIDIClock_TransportMaster::locked () const
{
	return true;
}

bool
MIDIClock_TransportMaster::ok() const
{
	return true;
}

bool
MIDIClock_TransportMaster::starting() const
{
	return false;
}

ARDOUR::samplecnt_t
MIDIClock_TransportMaster::resolution() const
{
	// one beat
	return (samplecnt_t) one_ppqn_in_samples * ppqn;
}

std::string
MIDIClock_TransportMaster::position_string () const
{
	return std::string();
}

std::string
MIDIClock_TransportMaster::delta_string() const
{
	char delta[80];
	if (last_timestamp == 0 || starting()) {
		snprintf(delta, sizeof(delta), "\u2012\u2012\u2012\u2012");
	} else {
		snprintf(delta, sizeof(delta), "\u0394<span foreground=\"green\" face=\"monospace\" >%s%s%" PRIi64 "</span>sm",
				LEADINGZERO(abs(_current_delta)), PLUSMINUS(-_current_delta), abs(_current_delta));
	}
	return std::string(delta);
}
