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

#include <iostream>

#include "vst3/vst3.h"

#include "ardour/vst3_module.h"
#include "ardour/vst3_host.h"
#include "ardour/vst3_scan.h"

using namespace std;
using namespace Steinberg;

static int32
count_channels (Vst::IComponent* c, Vst::MediaType media, Vst::BusDirection dir, Vst::BusType type)
{
	int32 n_busses = c->getBusCount (media, dir);
	for (int32 i = 0; i < n_busses; ++i) {
		Vst::BusInfo bus;
		if (c->getBusInfo (media, dir, i, bus) == kResultTrue && bus.busType == type) {
			if ((type == Vst::kMain && i != 0) || (type == Vst::kAux && i != 1)) {
				std::cerr << "Invaldid bus index\n";
				return 0;
			}
			return bus.channelCount;
		}
	}
	return 0;
}

bool
ARDOUR::discover_vst3 (boost::shared_ptr<VST3PluginModule> m, std::vector<VST3Info>& rv)
{
	using namespace std;

	GetFactoryProc fp = (GetFactoryProc)m->fn_ptr ("GetPluginFactory");

	if (!fp) {
		cerr << "Failed to find 'GetPluginFactory' function\n";
		return false;
	}

	IPluginFactory* factory = fp ();

	if (!factory) {
		cerr << "Failed to get VST3 plug-in factory\n";
		return false;
	}

	int32 class_cnt = factory->countClasses ();
	for (int32 i = 0; i < class_cnt; ++i) {
		PClassInfo ci;
		if (factory->getClassInfo (i, &ci) == kResultTrue) {
			if (strcmp (ci.category, kVstAudioEffectClass)) {
				continue;
			}

			VST3Info nfo;
			TUID     uid;

			//cout << "FOUND: " << i << " '" << ci.name << "'  '" << ci.category << "'" << "\n";

			PClassInfo2      ci2;
			IPluginFactory2* f2 = FUnknownPtr<IPluginFactory2> (factory);
			if (f2 && f2->getClassInfo2 (i, &ci2) == kResultTrue) {
				memcpy (uid, ci2.cid, sizeof (TUID));
				nfo.name       = ci2.name;
				nfo.vendor     = ci2.vendor;
				nfo.category   = ci2.subCategories;
				nfo.version    = ci2.version;
				nfo.sdkVersion = ci2.sdkVersion;
			} else {
				memcpy (uid, ci.cid, sizeof (TUID));
				nfo.name       = ci.name;
				nfo.vendor     = "Unknown";
				nfo.category   = "";
				nfo.version    = "0.0.0";
				nfo.sdkVersion = "VST 3";
			}

			nfo.index = i;
			{
				char suid[33] = "";
				FUID::fromTUID (uid).toString (suid);
				nfo.uid = suid;
			}

			Vst::IComponent* component;
			if (factory->createInstance (uid, Vst::IComponent::iid, (void**)&component) != kResultTrue) {
				cerr << "Failed to create VST3 component\n";
				continue;
			}

			// TODO init params

			if (component->initialize (HostApplication::getHostContext ()) != kResultOk) {
				cerr << "Failed to initialize VST3 component\n";
				//component->terminate();
				continue;
			}

			FUnknownPtr<Vst::IAudioProcessor> processor;
			if (!(processor = FUnknownPtr<Vst::IAudioProcessor> (component))) {
				cerr << "VST3: No valid processor";
				//controller->terminate();
				component->terminate ();
				continue;
			}

			nfo.n_inputs       = count_channels (component, Vst::kAudio, Vst::kInput,  Vst::kMain);
			nfo.n_aux_inputs   = count_channels (component, Vst::kAudio, Vst::kInput,  Vst::kAux);
			nfo.n_outputs      = count_channels (component, Vst::kAudio, Vst::kOutput, Vst::kMain);
			nfo.n_aux_outputs  = count_channels (component, Vst::kAudio, Vst::kOutput, Vst::kAux);
			nfo.n_midi_inputs  = count_channels (component, Vst::kEvent, Vst::kInput,  Vst::kMain);
			nfo.n_midi_outputs = count_channels (component, Vst::kEvent, Vst::kOutput, Vst::kMain);

			//controller->terminate();
			component->terminate ();
			rv.push_back (nfo);
		}
	}
	return true;
}
