#include "mavlink_parameters.h"
#include "system_impl.h"
#include <future>

namespace dronecode_sdk {

MAVLinkParameters::MAVLinkParameters(SystemImpl &parent) : _parent(parent)
{
    _parent.register_mavlink_message_handler(
        MAVLINK_MSG_ID_PARAM_VALUE,
        std::bind(&MAVLinkParameters::process_param_value, this, std::placeholders::_1),
        this);

    _parent.register_mavlink_message_handler(
        MAVLINK_MSG_ID_PARAM_EXT_VALUE,
        std::bind(&MAVLinkParameters::process_param_ext_value, this, std::placeholders::_1),
        this);

    _parent.register_mavlink_message_handler(
        MAVLINK_MSG_ID_PARAM_EXT_ACK,
        std::bind(&MAVLinkParameters::process_param_ext_ack, this, std::placeholders::_1),
        this);
}

MAVLinkParameters::~MAVLinkParameters()
{
    _parent.unregister_all_mavlink_message_handlers(this);
}

void MAVLinkParameters::set_param_async(const std::string &name,
                                        const ParamValue &value,
                                        set_param_callback_t callback,
                                        bool extended)
{
    // if (value.is_float()) {
    //     LogDebug() << "setting param " << name << " to " << value.get_float();
    // } else {
    //     LogDebug() << "setting param " << name << " to " << value.get_int();
    // }

    if (name.size() > PARAM_ID_LEN) {
        LogErr() << "Error: param name too long";
        if (callback) {
            callback(Result::PARAM_NAME_TOO_LONG);
        }
        return;
    }

    SetParamWork new_work;
    new_work.callback = callback;
    new_work.param_name = name;
    new_work.param_value = value;
    new_work.extended = extended;

    _set_param_queue.push_back(new_work);
}

MAVLinkParameters::Result
MAVLinkParameters::set_param(const std::string &name, const ParamValue &value, bool extended)
{
    auto prom = std::promise<Result>();
    auto res = prom.get_future();

    set_param_async(name, value, [&prom](Result result) { prom.set_value(result); }, extended);

    return res.get();
}

void MAVLinkParameters::get_param_async(const std::string &name,
                                        ParamValue value_type,
                                        get_param_callback_t callback,
                                        bool extended)
{
    // LogDebug() << "getting param " << name << ", extended: " << (extended ? "yes" : "no");

    if (name.size() > PARAM_ID_LEN) {
        LogErr() << "Error: param name too long";
        if (callback) {
            ParamValue empty_param;
            callback(MAVLinkParameters::Result::PARAM_NAME_TOO_LONG, empty_param);
        }
        return;
    }

    // Use cached value if available.
    if (_cache.find(name) != _cache.end()) {
        if (callback) {
            callback(MAVLinkParameters::Result::SUCCESS, _cache[name]);
        }
        return;
    }

    // Otherwise push work onto queue.
    GetParamWork new_work{};
    new_work.callback = callback;
    new_work.param_name = name;
    new_work.param_value_type = value_type;
    new_work.extended = extended;

    _get_param_queue.push_back(new_work);
}

std::pair<MAVLinkParameters::Result, MAVLinkParameters::ParamValue>
MAVLinkParameters::get_param(const std::string &name, ParamValue value_type, bool extended)
{
    auto prom = std::promise<std::pair<Result, MAVLinkParameters::ParamValue>>();
    auto res = prom.get_future();

    get_param_async(name,
                    value_type,
                    [&prom](Result result, ParamValue value) {
                        prom.set_value(std::make_pair<>(result, value));
                    },
                    extended);

    return res.get();
}

void MAVLinkParameters::do_work()
{
    auto set_param_work = _set_param_queue.borrow_front();

    if (set_param_work) {
        if (!set_param_work->already_requested) {
            char param_id[PARAM_ID_LEN + 1] = {};
            STRNCPY(param_id, set_param_work->param_name.c_str(), sizeof(param_id) - 1);

            mavlink_message_t message = {};
            if (set_param_work->extended) {
                char param_value_buf[128] = {};
                set_param_work->param_value.get_128_bytes(param_value_buf);

                // FIXME: extended currently always go to the camera component
                mavlink_msg_param_ext_set_pack(
                    GCSClient::system_id,
                    GCSClient::component_id,
                    &message,
                    _parent.get_system_id(),
                    MAV_COMP_ID_CAMERA,
                    param_id,
                    param_value_buf,
                    set_param_work->param_value.get_mav_param_ext_type());
            } else {
                // Param set is intended for Autopilot only.
                mavlink_msg_param_set_pack(GCSClient::system_id,
                                           GCSClient::component_id,
                                           &message,
                                           _parent.get_system_id(),
                                           _parent.get_autopilot_id(),
                                           param_id,
                                           set_param_work->param_value.get_4_float_bytes(),
                                           set_param_work->param_value.get_mav_param_type());
            }

            if (!_parent.send_message(message)) {
                LogErr() << "Error: Send message failed";
                if (set_param_work->callback) {
                    set_param_work->callback(MAVLinkParameters::Result::CONNECTION_ERROR);
                }
                _set_param_queue.pop_front();
                return;
            }

            set_param_work->already_requested = true;

            // _last_request_time = _parent.get_time().steady_time();

            // We want to get notified if a timeout happens
            _parent.register_timeout_handler(
                std::bind(&MAVLinkParameters::receive_timeout, this), 0.5, &_timeout_cookie);

            _set_param_queue.return_front();
        } else {
            _set_param_queue.return_front();
        }
    }

    auto get_param_work = _get_param_queue.borrow_front();
    if (get_param_work) {
        if (!get_param_work->already_requested) {
            char param_id[PARAM_ID_LEN + 1] = {};
            STRNCPY(param_id, get_param_work->param_name.c_str(), sizeof(param_id) - 1);

            // LogDebug() << "now getting: " << get_param_work->param_name;

            mavlink_message_t message = {};
            if (get_param_work->extended) {
                mavlink_msg_param_ext_request_read_pack(GCSClient::system_id,
                                                        GCSClient::component_id,
                                                        &message,
                                                        _parent.get_system_id(),
                                                        MAV_COMP_ID_CAMERA,
                                                        param_id,
                                                        -1);

            } else {
                // LogDebug() << "request read: "
                //    << (int)GCSClient::system_id << ":"
                //    << (int)GCSClient::component_id <<
                //    " to "
                //    << (int)_parent.get_system_id() << ":"
                //    << (int)_parent.get_autopilot_id();

                mavlink_msg_param_request_read_pack(GCSClient::system_id,
                                                    GCSClient::component_id,
                                                    &message,
                                                    _parent.get_system_id(),
                                                    _parent.get_autopilot_id(),
                                                    param_id,
                                                    -1);
            }

            if (!_parent.send_message(message)) {
                LogErr() << "Error: Send message failed";
                if (get_param_work->callback) {
                    ParamValue empty_param;
                    get_param_work->callback(MAVLinkParameters::Result::CONNECTION_ERROR,
                                             empty_param);
                }
                _get_param_queue.pop_front();
                return;
            }

            get_param_work->already_requested = true;

            // _last_request_time = _parent.get_time().steady_time();

            // We want to get notified if a timeout happens
            _parent.register_timeout_handler(
                std::bind(&MAVLinkParameters::receive_timeout, this), 0.5, &_timeout_cookie);

            _get_param_queue.return_front();
        } else {
            _get_param_queue.return_front();
        }
    }
}

void MAVLinkParameters::reset_cache()
{
    _cache.clear();
}

void MAVLinkParameters::process_param_value(const mavlink_message_t &message)
{
    mavlink_param_value_t param_value;
    mavlink_msg_param_value_decode(&message, &param_value);

    // LogDebug() << "getting param value: " << param_value.param_id;

    auto get_param_work = _get_param_queue.borrow_front();

    if (get_param_work) {
        if (get_param_work->already_requested) {
            if (strncmp(get_param_work->param_name.c_str(), param_value.param_id, PARAM_ID_LEN) ==
                0) {
                ParamValue value;
                value.set_from_mavlink_param_value(param_value);
                if (value.is_same_type(get_param_work->param_value_type)) {
                    _cache[get_param_work->param_name] = value;
                    if (get_param_work->callback) {
                        get_param_work->callback(MAVLinkParameters::Result::SUCCESS, value);
                    }
                } else {
                    LogErr() << "Param types don't match";
                    ParamValue no_value;
                    if (get_param_work->callback) {
                        get_param_work->callback(MAVLinkParameters::Result::WRONG_TYPE, no_value);
                    }
                }
                _parent.unregister_timeout_handler(_timeout_cookie);
                // LogDebug() << "time taken: " <<
                // _parent.get_time().elapsed_since_s(_last_request_time);
                _get_param_queue.pop_front();
            } else {
                // No match, let's just return the borrowed work item.
                _get_param_queue.return_front();
            }
        } else {
            // Nevermind, the request is not event sent yet.
            _get_param_queue.return_front();
        }
        return;
    }

    auto set_param_work = _set_param_queue.borrow_front();
    if (set_param_work) {
        if (set_param_work->already_requested) {
            // Now it still needs to match the param name
            if (strncmp(set_param_work->param_name.c_str(), param_value.param_id, PARAM_ID_LEN) ==
                0) {
                // We are done, inform caller and go back to idle
                _cache[set_param_work->param_name] = set_param_work->param_value;
                if (set_param_work->callback) {
                    set_param_work->callback(MAVLinkParameters::Result::SUCCESS);
                }

                _parent.unregister_timeout_handler(_timeout_cookie);
                // LogDebug() << "time taken: " <<
                // _parent.get_time().elapsed_since_s(_last_request_time);
                _set_param_queue.pop_front();
            } else {
                _set_param_queue.return_front();
            }
        } else {
            // Nevermind, the request is not event sent yet.
            _set_param_queue.return_front();
        }
        return;
    }
}

void MAVLinkParameters::process_param_ext_value(const mavlink_message_t &message)
{
    // LogDebug() << "getting param ext value";
    mavlink_param_ext_value_t param_ext_value;
    mavlink_msg_param_ext_value_decode(&message, &param_ext_value);

    auto get_param_work = _get_param_queue.borrow_front();

    if (get_param_work) {
        if (get_param_work->already_requested) {
            if (strncmp(get_param_work->param_name.c_str(),
                        param_ext_value.param_id,
                        PARAM_ID_LEN) == 0) {
                ParamValue value;
                value.set_from_mavlink_param_ext_value(param_ext_value);
                if (value.is_same_type(get_param_work->param_value_type)) {
                    _cache[get_param_work->param_name] = value;
                    if (get_param_work->callback) {
                        get_param_work->callback(MAVLinkParameters::Result::SUCCESS, value);
                    }
                } else {
                    LogErr() << "Param types don't match";
                    ParamValue no_value;
                    if (get_param_work->callback) {
                        get_param_work->callback(MAVLinkParameters::Result::WRONG_TYPE, no_value);
                    }
                }
                _parent.unregister_timeout_handler(_timeout_cookie);
                // LogDebug() << "time taken: " <<
                // _parent.get_time().elapsed_since_s(_last_request_time);
                _get_param_queue.pop_front();
            } else {
                _get_param_queue.return_front();
            }
        } else {
            _get_param_queue.return_front();
        }
    }
}

void MAVLinkParameters::process_param_ext_ack(const mavlink_message_t &message)
{
    // LogDebug() << "getting param ext ack";

    mavlink_param_ext_ack_t param_ext_ack;
    mavlink_msg_param_ext_ack_decode(&message, &param_ext_ack);

    auto set_param_work = _set_param_queue.borrow_front();
    if (set_param_work) {
        if (set_param_work->already_requested) {
            // Now it still needs to match the param name
            if (strncmp(set_param_work->param_name.c_str(), param_ext_ack.param_id, PARAM_ID_LEN) ==
                0) {
                if (param_ext_ack.param_result == PARAM_ACK_ACCEPTED) {
                    // We are done, inform caller and go back to idle
                    _cache[set_param_work->param_name] = set_param_work->param_value;
                    if (set_param_work->callback) {
                        set_param_work->callback(MAVLinkParameters::Result::SUCCESS);
                    }

                    _parent.unregister_timeout_handler(_timeout_cookie);
                    // LogDebug() << "time taken: " <<
                    // _parent.get_time().elapsed_since_s(_last_request_time);
                    _set_param_queue.pop_front();

                } else if (param_ext_ack.param_result == PARAM_ACK_IN_PROGRESS) {
                    // Reset timeout and wait again.
                    _parent.refresh_timeout_handler(_timeout_cookie);
                    _set_param_queue.return_front();

                } else {
                    LogErr() << "Somehow we did not get an ack, we got: "
                             << int(param_ext_ack.param_result);

                    if (set_param_work->callback) {
                        set_param_work->callback(MAVLinkParameters::Result::TIMEOUT);
                    }

                    _parent.unregister_timeout_handler(_timeout_cookie);
                    // LogDebug() << "time taken: " <<
                    // _parent.get_time().elapsed_since_s(_last_request_time);
                    _set_param_queue.pop_front();
                }
            } else {
                _set_param_queue.return_front();
            }
        } else {
            _set_param_queue.return_front();
        }
    }
}

void MAVLinkParameters::receive_timeout()
{
    auto get_param_work = _get_param_queue.borrow_front();
    if (get_param_work) {
        if (get_param_work->already_requested) {
            if (get_param_work->callback) {
                ParamValue empty_value;
                // Notify about timeout
                LogErr() << "Error: get param busy timeout: " << get_param_work->param_name;
                // LogErr() << "Got it after: " <<
                // _parent.get_time().elapsed_since_s(_last_request_time);
                get_param_work->callback(MAVLinkParameters::Result::TIMEOUT, empty_value);
            }
            // TODO: we should retry!
            _get_param_queue.pop_front();
        } else {
            _get_param_queue.return_front();
        }
    }

    auto set_param_work = _set_param_queue.borrow_front();
    if (set_param_work) {
        if (set_param_work->already_requested) {
            if (set_param_work->callback) {
                // Notify about timeout
                LogErr() << "Error: set param busy timeout: " << set_param_work->param_name;
                // LogErr() << "Got it after: " <<
                // _parent.get_time().elapsed_since_s(_last_request_time);
                set_param_work->callback(MAVLinkParameters::Result::TIMEOUT);
            }
            // TODO: we should retry!
            _set_param_queue.pop_front();
        } else {
            _set_param_queue.return_front();
        }
    }
}

std::ostream &operator<<(std::ostream &strm, const MAVLinkParameters::ParamValue &obj)
{
    strm << obj.get_string();
    return strm;
}

} // namespace dronecode_sdk
