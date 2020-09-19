#include "Context.h"

#include "ops/broadcast.h"

#include "ops/nodes.h"
#include "ops/strings.h"

#include "types/Vector.h"
#include "types/Color.h"

#ifndef M_MIN
#define M_MIN(_a, _b) ((_a)<(_b)?(_a):(_b))
#endif

Context::Context() {
}

Context::~Context() {
}

bool Context::load(const std::string& _filename) {
    config = YAML::LoadFile(_filename);

    // JS Globals
    JSValue global = parseNode(js, config["global"]);
    js.setGlobalValue("global", std::move(global));

    // Load MidiDevices
    std::vector<std::string> availableDevices = MidiDevice::getInputPorts();

    // Define out targets
    if (config["out"].IsSequence()) {
        for (size_t i = 0; i < config["out"].size(); i++) {
            std::string name = config["out"][i].as<std::string>();
            Target target = parseTarget( name );

            if (target.protocol == MIDI_PROTOCOL) {
                MidiDevice* m = new MidiDevice(this, name);

                int deviceID = getMatchingKey(availableDevices, target.address);
                if (deviceID >= 0)
                    m->openOutPort(target.address, deviceID);
                else
                    m->openVirtualOutPort(target.address);

                targetsDevicesNames.push_back( target.address );
                targetsDevices[target.address] = (Device*)m;
            }
            
            targets.push_back(target);
        }
    }

    // JS Functions
    uint32_t id = 0;

    if (config["in"].IsMap()) {
        for (YAML::const_iterator dev = config["in"].begin(); dev != config["in"].end(); ++dev) {
            std::string inName = dev->first.as<std::string>();
            int deviceID = getMatchingKey(availableDevices, inName);

            if (deviceID >= 0) {
                MidiDevice* m = new MidiDevice(this, inName, deviceID);
                devicesNames.push_back(inName);
                devices[inName] = (Device*)m;

                for (size_t i = 0; i < config["in"][inName].size(); i++) {

                    if (config["in"][inName][i]["key"].IsDefined()) {

                        bool haveShapingFunction = false;
                        if (config["in"][inName][i]["shape"].IsDefined()) {
                            std::string function = config["in"][inName][i]["shape"].as<std::string>();
                            if ( js.setFunction(id, function) ) {
                                haveShapingFunction = true;
                            }

                        }

                        if (config["in"][inName][i]["key"].IsScalar()) {
                            size_t key = config["in"][inName][i]["key"].as<size_t>();
                            std::cout << " adding key " << key << std::endl;

                            devices[inName]->keyMap[key] = i;
                            // std::cout << " linking " << (inName + "_" + toString(key)) << " w id " << id << std::endl; 
                            if (haveShapingFunction)
                                shapeFncs[inName + "_" + toString(key)] = id;
                        }
                        else if (config["in"][inName][i]["key"].IsSequence()) {
                            for (size_t j = 0; j < config["in"][inName][i]["key"].size(); j++) {
                                size_t key = config["in"][inName][i]["key"][j].as<size_t>();
                                std::cout << " adding key " << key << std::endl;

                                devices[inName]->keyMap[key] = i;
                                // std::cout << " linking " << (inName + "_" + toString(key)) << " w id " << id << std::endl; 
                                if (haveShapingFunction)
                                    shapeFncs[inName + "_" + toString(key)] = id;
                            }
                        }
                            
                        if (haveShapingFunction)
                            id++;
                    }   
                }
            
                updateDevice(inName);
            }
        }
    }

    if (devices.size() == 0) {
        std::cout << "Listening to no midi device. Please setup one of the following one in your config YAML file: " << std::endl;
        for (size_t i = 0; i < availableDevices.size(); i++)
            std::cout << "  - " << availableDevices[i] << std::endl;
    }

    // Load Pulses
    if (config["pulse"].IsSequence()) {
        for (size_t i = 0; i < config["pulse"].size(); i++) {
            YAML::Node n = config["pulse"][i];
            std::string name = n["name"].as<std::string>();

            // std::cout << "Adding pulse: " << name << std::endl;
            Pulse* p = new Pulse(this, i);
            
            if (n["bpm"].IsDefined())
                p->start(60000/n["bpm"].as<int>());
            else if (n["fps"].IsDefined()) 
                p->start(1000/int(n["fps"].as<float>()) );
            else if (n["interval"].IsDefined()) 
                p->start(int(n["interval"].as<float>()));

            if (n["shape"].IsDefined()) {
                std::string function = n["shape"].as<std::string>();
                if ( js.setFunction(id, function) ) {
                    shapeFncs[name + "_0"] = id;
                    id++;
                }
            }

            devicesNames.push_back(name);
            devices[name] = (Device*)p;
        }
    }

    return true;
}

bool Context::save(const std::string& _filename) {
    YAML::Emitter out;
    out.SetIndent(4);
    out.SetSeqFormat(YAML::Flow);
    out << config;

    std::ofstream fout(_filename);
    fout << out.c_str();

    return true;
}

bool Context::close() {
    for (std::map<std::string, Device*>::iterator it = devices.begin(); it != devices.end(); it++) {
        if (it->second->type == DEVICE_MIDI) {
            delete ((MidiDevice*)it->second);
        }
        else if (it->second->type == DEVICE_PULSE) {
            ((Pulse*)it->second)->stop();
            delete ((Pulse*)it->second);
        }
    }
    
    devices.clear();
    devicesNames.clear();

    shapeFncs.clear();

    targets.clear();
    targetsDevices.clear();
    targetsDevicesNames.clear();
    
    config = YAML::Node();

    return true;
}

bool Context::updateDevice(const std::string& _device) {

    for (size_t i = 0; i < config["in"][_device].size(); i++) {
        size_t key = i;

        if (config["in"][_device][i]["key"].IsDefined()) {
            if (config["in"][_device][i]["key"].IsScalar()) {
                size_t key = config["in"][_device][i]["key"].as<size_t>();
                updateKey(config["in"][_device][i], _device, key);
            }
            else if (config["in"][_device][i]["key"].IsSequence()) {
                for (size_t j = 0; j < config["in"][_device][i]["key"].size(); j++) {
                    size_t key = config["in"][_device][i]["key"][j].as<size_t>();
                    updateKey(config["in"][_device][i], _device, key);
                }
            }
        }
        else {
            updateKey(config["in"][_device][i], _device, i);
        }
    }

    return true;
}

bool Context::doKeyExist(const std::string& _device, size_t _key) {
    // if (_device == "ortho remote") {
    //     return true;
    // }
    // else 
    if ( !config["in"][_device].IsNull() ) {

        if (config["in"][_device].IsMap()) {
            // std::cout << " exploring the map for the KEY" << _key << std::endl; 
            std::string key = toString(_key);
            if (config["in"][_device][key].IsDefined())
                return true;
        }

        else if (config["in"][_device].IsSequence()) {
            // std::cout << " exploring the sequence (" << devices[_device]->keyMap.size() << ") for the KEY " << _key << std::endl;
            return devices[_device]->keyMap.find(_key) != devices[_device]->keyMap.end(); 
        }
    }
    return false;
}

YAML::Node  Context::getKeyNode(const std::string& _device, size_t _key) {
    if (config["in"][_device].IsMap()) {
        std::string key = toString(_key);
        return config["in"][_device][key];
    }

    else if (config["in"][_device].IsSequence()) {
        size_t i = devices[_device]->keyMap[_key];
        // std::cout << "ID: " << i << std::endl;
        return config["in"][_device][i];
    }

    return YAML::Node();
}

DataType Context::getKeyDataType(YAML::Node _keynode) {
    if ( _keynode.IsDefined() ) {
        if ( _keynode["type"].IsDefined() ) {
            std::string typeString =  _keynode["type"].as<std::string>();

            if (typeString == "button")
                return TYPE_BUTTON;
            else if (typeString == "toggle")
                return TYPE_TOGGLE;

            else if (   typeString == "state" ||
                        typeString == "enum" ||
                        typeString == "strings" )
                return TYPE_STRING;

            else if (   typeString == "scalar" ||
                        typeString == "number" ||
                        typeString == "float" ||
                        typeString == "int" )
                return TYPE_NUMBER;
                
            else if (   typeString == "vec2" ||
                        typeString == "vec3" ||
                        typeString == "vector" )
                return TYPE_VECTOR;
            else if (   typeString == "vec4" ||
                        typeString == "color" )
                return TYPE_COLOR;

            else if (   typeString == "note" )
                return TYPE_MIDI_NOTE;
            else if (   typeString == "cc" )
                return TYPE_MIDI_CONTROLLER_CHANGE;
            else if (   typeString == "tick" )
                return TYPE_MIDI_TIMING_TICK;
        }
    }
    return TYPE_NUMBER;
}

std::string Context::getKeyName(YAML::Node _keynode) {
    if ( _keynode.IsDefined() ) {
        if ( _keynode["name"].IsDefined() ) {
            return _keynode["name"].as<std::string>();
        }
    }
    return "unknownName";
}

bool Context::shapeKeyValue(YAML::Node _keynode, const std::string& _device, const std::string& _type, size_t _key, float* _value) {
    if ( _keynode["shape"].IsDefined() ) {
        js.setGlobalValue("device", js.newString(_device));
        js.setGlobalValue("type", js.newString(_type));
        js.setGlobalValue("key", js.newNumber(_key));
        js.setGlobalValue("value", js.newNumber(*_value));

        JSValue keyData = parseNode(js, _keynode);
        js.setGlobalValue("data", std::move(keyData));

        std::string key = toString(_key);
        JSValue result = js.getFunctionResult( shapeFncs[_device + "_" + key] );

        if (!result.isNull()) {

            if (result.isString()) {
                std::cout << "Update result on string: " << result.toString() << " but don't know what to do with it"<< std::endl;
                return false;
            }

            else if (result.isObject()) {
                // std::cout << "shapeKey return Object" << std::endl;
                for (size_t j = 0; j < targetsDevicesNames.size(); j++) {
                    JSValue d = result.getValueForProperty(targetsDevicesNames[j]);
                    if (!d.isUndefined()) {
                        for (size_t i = 0; i < d.getLength(); i++) {
                            JSValue el = d.getValueAtIndex(i);
                            if (el.isArray()) {
                                if (el.getLength() > 1) {

                                    MidiDevice* d = (MidiDevice*)targetsDevices[ targetsDevicesNames[j] ];
                                    size_t k = el.getValueAtIndex(0).toInt();
                                    size_t v = el.getValueAtIndex(1).toInt();
                                    // d->send( MidiDevice::CONTROLLER_CHANGE, k, v );
                                    d->send( MidiDevice::NOTE_ON, k, v );
                                }
                            }
                        }
                    }
                }

                for (size_t j = 0; j < devicesNames.size(); j++) {
                    JSValue d = result.getValueForProperty(devicesNames[j]);

                    if (!d.isUndefined()) {
                        for (size_t i = 0; i < d.getLength(); i++) {
                            JSValue el = d.getValueAtIndex(i);
                            if (el.isArray()) {
                                if (el.getLength() > 1) {
                                    size_t k = el.getValueAtIndex(0).toInt();
                                    float v = el.getValueAtIndex(1).toFloat();
                                    // std::cout << "trigger(" << devicesNames[j] << "," << k << "," << v << ")" << std::endl;
                                    YAML::Node n = getKeyNode(devicesNames[j], k);
                                    mapKeyValue(n, devicesNames[j], k, v);
                                }
                            }
                        }
                    }

                    JSValue d_leds = result.getValueForProperty(devicesNames[j] + "_FEEDBACKLEDS");
                    if (!d_leds.isUndefined()) {
                        for (size_t i = 0; i < d_leds.getLength(); i++) {
                            JSValue el = d_leds.getValueAtIndex(i);
                            if (el.isArray()) {
                                if (el.getLength() > 1) {
                                    size_t k = el.getValueAtIndex(0).toInt();
                                    float v = el.getValueAtIndex(1).toFloat();
                                    YAML::Node n = getKeyNode(devicesNames[j], k);
                                    DataType n_type = getKeyDataType(n);

                                    // BUTTONs and TOGGLEs need to change state on the device
                                    if (devices[devicesNames[j]]->type == DEVICE_MIDI && 
                                         (n_type == TYPE_BUTTON || n_type == TYPE_TOGGLE)) {
                                        feedbackLED(devicesNames[j], k, v);
                                    }
                                }
                            }
                        }
                    }                 
                }
                return false;
            }

            else if (result.isArray()) {
                // std::cout << "shapeKey return Array" << std::endl;
                for (size_t i = 0; i < result.getLength(); i++) {
                    JSValue el = result.getValueAtIndex(i);
                    if (el.isArray()) {
                        if (el.getLength() == 2) {
                            size_t k = el.getValueAtIndex(0).toInt();
                            float v = el.getValueAtIndex(1).toFloat();
                            // std::cout << "trigger (" << _device << "," << k << "," << v << ")" << std::endl;
                            mapKeyValue(_keynode, _device, k, v);
                        }
                    }
                }
                return false;
            }
            
            else if (result.isNumber()) {
                // std::cout << "shapeKey return Number" << std::endl;
                *_value = result.toFloat();
            }

            else if (result.isBoolean()) {
                // std::cout << "shapeKey return Bool" << std::endl;
                return result.toBool();
            }
        }
    }

    return true;
}

bool Context::mapKeyValue(YAML::Node _keynode, const std::string& _device, size_t _key, float _value) {

    std::string name = getKeyName(_keynode);
    DataType type = getKeyDataType(_keynode);
    _keynode["value_raw"] = _value;

    // BUTTON
    if (type == TYPE_BUTTON) {
        bool value = _value > 0;
        _keynode["value"] = value;
        return updateKey(_keynode, _device, _key);
    }
    
    // TOGGLE
    else if ( type == TYPE_TOGGLE ) {
        if (_value > 0) {
            bool value = false;
                
            if (_keynode["value"])
                value = _keynode["value"].as<bool>();

            _keynode["value"] = !value;
            return updateKey(_keynode, _device, _key);
        }
    }

    // STATE
    else if ( type == TYPE_STRING ) {
        int value = (int)_value;
        std::string value_str = toString(value);

        if ( _keynode["map"] ) {
            if ( _keynode["map"].IsSequence() ) {
                float total = _keynode["map"].size();

                if (value == 127.0f) {
                    value_str = _keynode["map"][total-1].as<std::string>();
                } 
                else {
                    size_t index = (value / 127.0f) * _keynode["map"].size();
                    value_str = _keynode["map"][index].as<std::string>();
                } 
            }
        }
            
        _keynode["value"] = value_str;
        return updateKey(_keynode, _device, _key);
    }
    
    // SCALAR
    else if ( type == TYPE_NUMBER ) {
        float value = _value;

        if ( _keynode["map"] ) {
            value /= 127.0f;
            if ( _keynode["map"].IsSequence() ) {
                if ( _keynode["map"].size() > 1 ) {
                    float total = _keynode["map"].size() - 1;

                    size_t i_low = value * total;
                    size_t i_high = M_MIN(i_low + 1, size_t(total));
                    float pct = (value * total) - (float)i_low;
                    value = lerp(   _keynode["map"][i_low].as<float>(),
                                    _keynode["map"][i_high].as<float>(),
                                    pct );
                }
            }
        }
            
        _keynode["value"] = value;
        return updateKey(_keynode, _device, _key);
    }
    
    // VECTOR
    else if ( type == TYPE_VECTOR ) {
        float pct = _value / 127.0f;
        Vector value = Vector(0.0, 0.0, 0.0);

        if ( _keynode["map"] ) {
            if ( _keynode["map"].IsSequence() ) {
                if ( _keynode["map"].size() > 1 ) {
                    float total = _keynode["map"].size() - 1;

                    size_t i_low = pct * total;
                    size_t i_high = M_MIN(i_low + 1, size_t(total));

                    value = lerp(   _keynode["map"][i_low].as<Vector>(),
                                    _keynode["map"][i_high].as<Vector>(),
                                    (pct * total) - (float)i_low );
                }
            }
        }
            
        _keynode["value"] = value;
        return updateKey(_keynode, _device, _key);
    }

    // COLOR
    else if ( type == TYPE_COLOR ) {
        float pct = _value / 127.0f;
        Color value = Color(0.0, 0.0, 0.0);

        if ( _keynode["map"] ) {
            if ( _keynode["map"].IsSequence() ) {
                if ( _keynode["map"].size() > 1 ) {
                    float total = _keynode["map"].size() - 1;

                    size_t i_low = pct * total;
                    size_t i_high = M_MIN(i_low + 1, size_t(total));

                    value = lerp(   _keynode["map"][i_low].as<Color>(),
                                    _keynode["map"][i_high].as<Color>(),
                                    (pct * total) - (float)i_low );
                }
            }
        }
        
        _keynode["value"] = value;
        return updateKey(_keynode, _device, _key);
    }

    else if (   type == TYPE_MIDI_NOTE || 
                type == TYPE_MIDI_CONTROLLER_CHANGE ||
                type == TYPE_MIDI_TIMING_TICK ) {

        _keynode["value"] = int(_value);

        return updateKey(_keynode, _device, _key);
    }


    return false;
}

bool Context::updateKey(YAML::Node _keynode, const std::string& _device, size_t _key) {

    if ( _keynode["value"].IsDefined() ) {

        // DataType type = getKeyDataType(_keynode);

        // // BUTTONs and TOGGLEs need to change state on the device
        // if (devices[_device]->type == DEVICE_MIDI && 
        //     (type == TYPE_BUTTON || type == TYPE_TOGGLE)) {
        //     feedbackLED(_device, _key, _keynode["value"].as<bool>() ? 127 : 0);
        // }

        return sendKeyValue(_keynode, _device, _key);
    }
    
    return false;
}

bool Context::feedbackLED(const std::string& _device, size_t _key, size_t _value){
    MidiDevice* midi = static_cast<MidiDevice*>(devices[_device]);
    midi->send( MidiDevice::CONTROLLER_CHANGE, _key, _value);
    return true;
}

bool Context::sendKeyValue(YAML::Node _keynode, const std::string& _device, size_t _key) {

    if ( !_keynode["value"].IsDefined() )
        return false;



    // Define out targets
    std::vector<Target> keyTargets;
    if (_keynode["out"].IsDefined() ) {
        if (_keynode["out"].IsSequence()) 
            for (size_t i = 0; i < _keynode["out"].size(); i++) {
                Target target = parseTarget( _keynode["out"][i].as<std::string>() ); 
                keyTargets.push_back( target );
            }
        else if (_keynode["out"].IsScalar()) {
            Target target = parseTarget( _keynode["out"].as<std::string>() );
            keyTargets.push_back( target );
        }
    }
    else
        keyTargets = targets;
        
    // KEY
    std::string name = getKeyName(_keynode);
    DataType type = getKeyDataType(_keynode);
    YAML::Node value = _keynode["value"];

    // BUTTON and TOGGLE
    if ( type == TYPE_TOGGLE || type == TYPE_BUTTON ) {
        std::string value_str = (value.as<bool>()) ? "on" : "off";
        
        if (_keynode["map"]) {

            if (_keynode["map"][value_str]) {

                // If the end mapped string is a sequence
                if (_keynode["map"][value_str].IsSequence()) {
                    for (size_t i = 0; i < _keynode["map"][value_str].size(); i++) {
                        std::string prop = name;
                        std::string msg = "";

                        if ( parseString(_keynode["map"][value_str][i], prop, msg) ) {
                            for (size_t t = 0; t < keyTargets.size(); t++)
                                broadcast(keyTargets[t], prop, msg);
                        }

                    }
                }
                else {
                    std::string prop = name;
                    std::string msg = "";

                    if ( parseString(_keynode["map"][value_str], prop, msg) ) {
                        for (size_t t = 0; t < keyTargets.size(); t++)
                            broadcast(keyTargets[t], prop, msg);
                    }
                }

            }

        }
        else {
            for (size_t t = 0; t < keyTargets.size(); t++)
                broadcast(keyTargets[t], name, value_str);

        }

        if ( devices[_device]->type == DEVICE_MIDI )
            feedbackLED(_device, _key, _keynode["value"].as<bool>() ? 127 : 0);

        return true;
    }

    // STATE
    else if ( type == TYPE_STRING ) {
        for (size_t t = 0; t < keyTargets.size(); t++)
            broadcast(keyTargets[t], name, value.as<std::string>());

        return true;
    }

    // SCALAR
    else if ( type == TYPE_NUMBER ) {
        for (size_t t = 0; t < keyTargets.size(); t++)
            broadcast(keyTargets[t], name, value.as<float>());

        return true;
    }

    // VECTOR
    else if ( type == TYPE_VECTOR ) {
        for (size_t t = 0; t < keyTargets.size(); t++)
            broadcast(keyTargets[t], name, value.as<Vector>());

        return true;
    }

    // COLOR
    else if ( type == TYPE_COLOR ) {
        for (size_t t = 0; t < keyTargets.size(); t++)
            broadcast(keyTargets[t], name, value.as<Color>());
        
        return true;
    }
    else if (targetsDevices.size() > 0) {
        size_t value = _keynode["value"].as<int>();

        for (size_t t = 0; t < keyTargets.size(); t++) {
            if (keyTargets[t].protocol == MIDI_PROTOCOL) {
                std::string name = keyTargets[t].address;

                if ( targetsDevices.find( name ) == targetsDevices.end() ) {
                    MidiDevice* d = (MidiDevice*)targetsDevices[ name ];

                    if ( type == TYPE_MIDI_NOTE) {
                        if (value == 0)
                            d->send( MidiDevice::NOTE_OFF, _key, 0 );
                        else 
                            d->send( MidiDevice::NOTE_ON, _key, value );
                    }
                    else if ( type == TYPE_MIDI_CONTROLLER_CHANGE ) {
                        d->send( MidiDevice::CONTROLLER_CHANGE, _key, value );
                    }
                    else if ( type == TYPE_MIDI_TIMING_TICK ) {
                        d->send( MidiDevice::TIMING_TICK );
                    }
                }
            }
        }
    }

    return false;
}


