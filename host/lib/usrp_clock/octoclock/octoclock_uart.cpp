//
// Copyright 2014 Ettus Research LLC
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#include <iostream>
#include <string>
#include <string.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/cstdint.hpp>
#include <boost/format.hpp>
#include <boost/thread/thread.hpp>

#include <uhd/exception.hpp>
#include <uhd/utils/byteswap.hpp>

#include "common.h"
#include "octoclock_uart.hpp"

namespace asio = boost::asio;
using namespace uhd::transport;

#define NUM_WRAPS_EQUAL   (_state.num_wraps == _device_state.num_wraps)
#define POS_EQUAL         (_state.pos == _device_state.pos)
#define STATES_EQUAL      (NUM_WRAPS_EQUAL && POS_EQUAL)
#define LOCAL_STATE_AHEAD (_state.num_wraps > _device_state.num_wraps || \
                           (NUM_WRAPS_EQUAL && _state.pos > _device_state.pos))

namespace uhd{
    octoclock_uart_iface::octoclock_uart_iface(udp_simple::sptr udp): uart_iface(){
        _udp = udp;
        _state.num_wraps = 0;
        _state.pos = 0;
        _device_state.num_wraps = 0;
        _device_state.pos = 0;
        size_t len = 0;

        //Get pool size from device
        octoclock_packet_t pkt_out;
        pkt_out.sequence = uhd::htonx<boost::uint16_t>(std::rand());
        pkt_out.len = 0;

        boost::uint8_t octoclock_data[udp_simple::mtu];
        const octoclock_packet_t *pkt_in = reinterpret_cast<octoclock_packet_t*>(octoclock_data);

        UHD_OCTOCLOCK_SEND_AND_RECV(_udp, SEND_POOLSIZE_CMD, pkt_out, len, octoclock_data);
        if(UHD_OCTOCLOCK_PACKET_MATCHES(SEND_POOLSIZE_ACK, pkt_out, pkt_in, len)){
            _poolsize = pkt_in->poolsize;
            _cache.resize(_poolsize);
        }
        else throw uhd::runtime_error("Failed to communicate with GPSDO.");
    }

    void octoclock_uart_iface::write_uart(const std::string &buf){
        std::string to_send = boost::algorithm::replace_all_copy(buf, "\n", "\r\n");
        size_t len = 0;

        octoclock_packet_t pkt_out;
        pkt_out.sequence = uhd::htonx<boost::uint32_t>(std::rand());
        pkt_out.len = to_send.size();
        memcpy(pkt_out.data, to_send.c_str(), to_send.size());

        boost::uint8_t octoclock_data[udp_simple::mtu];
        const octoclock_packet_t *pkt_in = reinterpret_cast<octoclock_packet_t*>(octoclock_data);

        UHD_OCTOCLOCK_SEND_AND_RECV(_udp, HOST_SEND_TO_GPSDO_CMD, pkt_out, len, octoclock_data);
        if(not UHD_OCTOCLOCK_PACKET_MATCHES(HOST_SEND_TO_GPSDO_ACK, pkt_out, pkt_in, len)){
            throw uhd::runtime_error("Failed to send commands to GPSDO.");
        }
    }

    std::string octoclock_uart_iface::read_uart(double timeout){
        std::string result;

        boost::system_time exit_time = boost::get_system_time() + boost::posix_time::milliseconds(long(timeout*1e3));

        while(boost::get_system_time() < exit_time){
            _update_cache();

            for(char ch = _getchar(); ch != -1; ch = _getchar()){
                if(ch == '\r') continue; //Skip carriage returns
                _rxbuff += ch;

                //If newline found, return string
                if(ch == '\n'){
                    result = _rxbuff;
                    _rxbuff.clear();
                    return result;
                }
            }
            boost::this_thread::sleep(boost::posix_time::milliseconds(1));
        }

        return result;
    }

    void octoclock_uart_iface::_update_cache(){
        octoclock_packet_t pkt_out;
        pkt_out.len = 0;
        size_t len = 0;

        boost::uint8_t octoclock_data[udp_simple::mtu];
        const octoclock_packet_t *pkt_in = reinterpret_cast<octoclock_packet_t*>(octoclock_data);

        if(STATES_EQUAL or LOCAL_STATE_AHEAD){
            pkt_out.sequence++;
            UHD_OCTOCLOCK_SEND_AND_RECV(_udp, SEND_GPSDO_CACHE_CMD, pkt_out, len, octoclock_data);
            if(UHD_OCTOCLOCK_PACKET_MATCHES(SEND_GPSDO_CACHE_ACK, pkt_out, pkt_in, len)){
                memcpy(&_cache[0], pkt_in->data, _poolsize);
                _device_state = pkt_in->state;
            }

            boost::uint8_t delta_wraps = (_device_state.num_wraps - _state.num_wraps);
            if(delta_wraps > 1 or
               ((delta_wraps == 1) and (_device_state.pos >= _state.pos))){

                _state.pos = (_device_state.pos+1) % _poolsize;
                _state.num_wraps = (_device_state.num_wraps-1);

                while((_cache[_state.pos] != '\n') and (_state.pos != _device_state.pos)){
                    _state.pos = (_state.pos+1) % _poolsize;
                    //We may have wrapped around locally
                    if(_state.pos == 0) _state.num_wraps++;
                }
                _state.pos = (_state.pos+1) % _poolsize;
                //We may have wrapped around locally
                if(_state.pos == 0) _state.num_wraps++;
            }
        }
    }

    char octoclock_uart_iface::_getchar(){
        if(LOCAL_STATE_AHEAD){
            return -1;
        }

        char ch = _cache[_state.pos];
        _state.pos = ((_state.pos+1) % _poolsize);
        //We may have wrapped around locally
        if(_state.pos == 0) _state.num_wraps++;

        return ch;
    }

    uart_iface::sptr octoclock_make_uart_iface(udp_simple::sptr udp){
        return uart_iface::sptr(new octoclock_uart_iface(udp));
    }
}
