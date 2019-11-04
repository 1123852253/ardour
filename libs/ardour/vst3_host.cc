/*
 * Copyright (C) 2019-2020 Robin Gareus <robin@gareus.org>
 * Copyright (C) 2019 Steinberg Media Technologies GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include <algorithm>

#if (__cplusplus >= 201103L)
#include <boost/make_unique.hpp>
#endif

#include "ardour/vst3_host.h"

using namespace Steinberg;

DEF_CLASS_IID (FUnknown)
DEF_CLASS_IID (Vst::IAttributeList)
DEF_CLASS_IID (Vst::IHostApplication)
DEF_CLASS_IID (Vst::IPlugInterfaceSupport)
DEF_CLASS_IID (Vst::IMessage)
DEF_CLASS_IID (Vst::IComponent)
DEF_CLASS_IID (Vst::IAudioProcessor)
DEF_CLASS_IID (Vst::IEditController)
DEF_CLASS_IID (Vst::IComponentHandler)
DEF_CLASS_IID (Vst::IConnectionPoint)
DEF_CLASS_IID (Vst::IUnitInfo)
DEF_CLASS_IID (Vst::IUnitData)
DEF_CLASS_IID (Vst::IProgramListData)
DEF_CLASS_IID (Vst::IMidiMapping)
DEF_CLASS_IID (Vst::IParameterChanges)
DEF_CLASS_IID (Vst::IParamValueQueue)
DEF_CLASS_IID (Vst::IEventList);
DEF_CLASS_IID (IPluginFactory2);
DEF_CLASS_IID (IPlugFrame);
DEF_CLASS_IID (IPlugView);

#if SMTG_OS_LINUX
DEF_CLASS_IID (Linux::IRunLoop);
#endif

std::string
Steinberg::tchar_to_utf8 (Vst::TChar const* s)
{
	glong  len;
	gchar* utf8 = g_utf16_to_utf8 ((const gunichar2*)s, -1, NULL, &len, NULL);
	if (!utf8 || len == 0) {
		return "";
	}
	std::string rv (utf8, len);
	g_free (utf8);
	return rv;
}

bool
Steinberg::utf8_to_tchar (Vst::TChar* rv, const char* s, size_t l)
{
	glong      len;
	gunichar2* s16 = g_utf8_to_utf16 (s, -1, NULL, &len, NULL);
	if (!s16 || len == 0) {
		memset (rv, 0, sizeof (Vst::TChar));
		return false;
	}
	if (l > 0 && l < len) {
		len = l;
	}
	memcpy (rv, s16, len * sizeof (Vst::TChar));
	g_free (s16);
	return true;
}

bool
Steinberg::utf8_to_tchar (Vst::TChar* rv, std::string const& s, size_t l)
{
	return utf8_to_tchar (rv, s.c_str(), l);
}

/* ****************************************************************************/

RefObject::RefObject ()
{
	g_atomic_int_set (&_cnt, 1);
}

uint32
RefObject::addRef ()
{
	g_atomic_int_inc (&_cnt);
	return g_atomic_int_get (&_cnt);
}

uint32
RefObject::release ()
{
	if (g_atomic_int_dec_and_test (&_cnt)) {
		delete this;
		return 0;
	}
	return g_atomic_int_get (&_cnt);
}

/* ****************************************************************************/
/* copy/edit from public.sdk/source/vst/hosting/hostclasses.cpp               */
/* ****************************************************************************/

#include "vst3/pluginterfaces/base/funknown.cpp"

HostAttributeList::HostAttributeList ()
{
}

HostAttributeList::~HostAttributeList ()
{
	std::map<std::string, HostAttribute*>::reverse_iterator it = list.rbegin ();
	while (it != list.rend ()) {
		delete it->second;
		it++;
	}
}

void
HostAttributeList::removeAttrID (AttrID aid)
{
	std::map<std::string, HostAttribute*>::iterator it = list.find (aid);
	if (it != list.end ()) {
		delete it->second;
		list.erase (it);
	}
}

tresult
HostAttributeList::setInt (AttrID aid, int64 value)
{
	removeAttrID (aid);
	list[aid] = new HostAttribute (value);
	return kResultTrue;
}

tresult
HostAttributeList::getInt (AttrID aid, int64& value)
{
	std::map<std::string, HostAttribute*>::iterator it = list.find (aid);
	if (it != list.end () && it->second) {
		value = it->second->intValue ();
		return kResultTrue;
	}
	return kResultFalse;
}

tresult
HostAttributeList::setFloat (AttrID aid, double value)
{
	removeAttrID (aid);
	list[aid] = new HostAttribute (value);
	return kResultTrue;
}

tresult
HostAttributeList::getFloat (AttrID aid, double& value)
{
	std::map<std::string, HostAttribute*>::iterator it = list.find (aid);
	if (it != list.end () && it->second) {
		value = it->second->floatValue ();
		return kResultTrue;
	}
	return kResultFalse;
}

tresult
HostAttributeList::setString (AttrID aid, const Vst::TChar* string)
{
	removeAttrID (aid);
	list[aid] = new HostAttribute (string, wcslen ((const wchar_t*)string));
	return kResultTrue;
}

tresult
HostAttributeList::getString (AttrID aid, Vst::TChar* string, uint32 size)
{
	std::map<std::string, HostAttribute*>::iterator it = list.find (aid);
	if (it != list.end () && it->second) {
		uint32            stringSize = 0;
		const Vst::TChar* _string    = it->second->stringValue (stringSize);
		memcpy (string, _string, std::min<uint32> (stringSize, size) * sizeof (Vst::TChar));
		return kResultTrue;
	}
	return kResultFalse;
}

tresult
HostAttributeList::setBinary (AttrID aid, const void* data, uint32 size)
{
	removeAttrID (aid);
	list[aid] = new HostAttribute (data, size);
	return kResultTrue;
}

tresult
HostAttributeList::getBinary (AttrID aid, const void*& data, uint32& size)
{
	std::map<std::string, HostAttribute*>::iterator it = list.find (aid);
	if (it != list.end () && it->second) {
		data = it->second->binaryValue (size);
		return kResultTrue;
	}
	size = 0;
	return kResultFalse;
}

/* ****************************************************************************/

HostMessage::HostMessage ()
	: _messageId (0)
{
}

HostMessage::~HostMessage ()
{
	setMessageID (0);
	if (_attribute_list) {
		//_attribute_list->release ();
	}
}

const char*
HostMessage::getMessageID ()
{
	return _messageId;
}

void
HostMessage::setMessageID (const char* mid)
{
	if (_messageId) {
		delete[] _messageId;
	}
	if (mid) {
		size_t len = strlen (mid) + 1;
		_messageId = new char[len];
		strcpy (_messageId, mid);
	} else {
		_messageId = 0;
	}
}

Vst::IAttributeList*
HostMessage::getAttributes ()
{
	if (!_attribute_list) {
		_attribute_list.reset (new HostAttributeList);
	}
	return _attribute_list.get ();
}

/* ****************************************************************************/

PlugInterfaceSupport::PlugInterfaceSupport ()
{
	using namespace Vst;

	//---VST 3.0.0--------------------------------
	addPlugInterfaceSupported (IComponent::iid);
	addPlugInterfaceSupported (IAudioProcessor::iid);
	addPlugInterfaceSupported (IEditController::iid);
	addPlugInterfaceSupported (IConnectionPoint::iid);

	addPlugInterfaceSupported (IUnitInfo::iid);
	addPlugInterfaceSupported (IUnitData::iid);
	addPlugInterfaceSupported (IProgramListData::iid);

	//---VST 3.0.1--------------------------------
	addPlugInterfaceSupported (IMidiMapping::iid);

#if 0
	//---VST 3.1----------------------------------
	addPlugInterfaceSupported (IEditController2::iid);

	//---VST 3.0.2--------------------------------
	addPlugInterfaceSupported (IParameterFinder::iid);

	//---VST 3.1----------------------------------
	addPlugInterfaceSupported (IAudioPresentationLatency::iid);

	//---VST 3.5----------------------------------
	addPlugInterfaceSupported (IKeyswitchController::iid);
	addPlugInterfaceSupported (IContextMenuTarget::iid);
	addPlugInterfaceSupported (IEditControllerHostEditing::iid);
	addPlugInterfaceSupported (IXmlRepresentationController::iid);
	addPlugInterfaceSupported (INoteExpressionController::iid);

	//---VST 3.6.5--------------------------------
	addPlugInterfaceSupported (ChannelContext::IInfoListener::iid);
	addPlugInterfaceSupported (IPrefetchableSupport::iid);
	addPlugInterfaceSupported (IAutomationState::iid);

	//---VST 3.6.11--------------------------------
	addPlugInterfaceSupported (INoteExpressionPhysicalUIMapping::iid);

	//---VST 3.6.12--------------------------------
	addPlugInterfaceSupported (IMidiLearn::iid);
#endif
}

tresult
PlugInterfaceSupport::isPlugInterfaceSupported (const TUID _iid)
{
	const FUID uid = FUID::fromTUID (_iid);
	if (std::find (_interfaces.begin (), _interfaces.end (), uid) != _interfaces.end ()) {
		return kResultTrue;
	}
	return kResultFalse;
}

void
PlugInterfaceSupport::addPlugInterfaceSupported (const TUID id)
{
	_interfaces.push_back (FUID::fromTUID (id));
}

/* ****************************************************************************/

HostApplication::HostApplication ()
{
#if (__cplusplus >= 201103L)
	_plug_interface_support = boost::make_unique<PlugInterfaceSupport> ();
#else
	_plug_interface_support.reset (new PlugInterfaceSupport);
#endif
}

tresult
HostApplication::queryInterface (const char* _iid, void** obj)
{
	QUERY_INTERFACE (_iid, obj, FUnknown::iid, IHostApplication)
	QUERY_INTERFACE (_iid, obj, IHostApplication::iid, IHostApplication)

	if (_plug_interface_support && _plug_interface_support->queryInterface (iid, obj) == kResultTrue) {
		return kResultOk;
	}

	*obj = nullptr;
	return kResultFalse;
}

tresult
HostApplication::getName (Vst::String128 name)
{
	utf8_to_tchar (name, "My VST3 HostApplication", 128);
	return kResultTrue;
}

tresult
HostApplication::createInstance (TUID cid, TUID _iid, void** obj)
{
	FUID classID (FUID::fromTUID (cid));
	FUID interfaceID (FUID::fromTUID (_iid));
	if (classID == Vst::IMessage::iid && interfaceID == Vst::IMessage::iid) {
		*obj = (Vst::IMessage*)new HostMessage;
		return kResultTrue;
	} else if (classID == Vst::IAttributeList::iid && interfaceID == Vst::IAttributeList::iid) {
		*obj = (Vst::IAttributeList*)new HostAttributeList;
		return kResultTrue;
	}
	*obj = nullptr;
	return kResultFalse;
}

/* ****************************************************************************/

tresult
Vst3ParamValueQueue::getPoint (int32 index, int32& sampleOffset, Vst::ParamValue& value)
{
	if (index >=0 && index < (int32)_values.size ()) {
		const Value& v = _values[index];
		sampleOffset = v.sampleOffset;
		value        = v.value;
		return kResultTrue;
	}
	return kResultFalse;

}

tresult
Vst3ParamValueQueue::addPoint (int32 sampleOffset, Vst::ParamValue value, int32& index)
{
	int32 dest_index = (int32)_values.size ();
	for (uint32 i = 0; i < _values.size (); ++i) {
		if (_values[i].sampleOffset == sampleOffset) {
			_values[i].value = value;
			index = i;
			return kResultTrue;
		} else if (_values[i].sampleOffset > sampleOffset) {
			dest_index = i;
			break;
		}
	}

	Value v (value, sampleOffset);
	if (dest_index == (int32)_values.size ()) {
		_values.push_back (v);
	} else {
		_values.insert (_values.begin () + dest_index, v);
	}

	index = dest_index;
	return kResultTrue;
}


Vst::IParamValueQueue*
Vst3ParameterChanges::getParameterData (int32 index)
{
  if (index < _used_queue_count) {
    return &_queue[index];
	}
  return 0;
}

Vst::IParamValueQueue*
Vst3ParameterChanges::addParameterData (Vst::ParamID const& pid, int32& index)
{
  for (int32 i = 0; i < _used_queue_count; ++i) {
    if (_queue[i].getParameterId () == pid) {
      index = i;
      return &_queue[i];
    }
  }

  if (_used_queue_count < (int32)_queue.size ()) {
		index = _used_queue_count++;
		_queue[index].setParameterId (pid);
		return &_queue[index];
  }
	index = 0;
	return 0;
}
