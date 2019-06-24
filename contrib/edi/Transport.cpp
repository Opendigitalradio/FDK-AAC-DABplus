/*
   Copyright (C) 2019
   Matthias P. Braendli, matthias.braendli@mpb.li

    http://www.opendigitalradio.org

   EDI output,
   UDP and TCP transports and their configuration

   */
/*
   This file is part of ODR-DabMux.

   ODR-DabMux is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as
   published by the Free Software Foundation, either version 3 of the
   License, or (at your option) any later version.

   ODR-DabMux is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with ODR-DabMux.  If not, see <http://www.gnu.org/licenses/>.
   */

#include "edi/Transport.h"
#include <iterator>
#include <iostream>

using namespace std;

namespace edi {

void configuration_t::print() const
{
    clog << "EDI" << endl;
    clog << " verbose     " << verbose << endl;
    for (auto edi_dest : destinations) {
        if (auto udp_dest = dynamic_pointer_cast<edi::udp_destination_t>(edi_dest)) {
            clog << " UDP to " << udp_dest->dest_addr << ":" << dest_port << endl;
            if (not udp_dest->source_addr.empty()) {
                clog << "  source      " << udp_dest->source_addr << endl;
                clog << "  ttl         " << udp_dest->ttl << endl;
            }
            clog << "  source port " << udp_dest->source_port << endl;
        }
        else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_server_t>(edi_dest)) {
            clog << " TCP listening on port " << tcp_dest->listen_port << endl;
            clog << "  max frames queued    " << tcp_dest->max_frames_queued << endl;
        }
        else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_client_t>(edi_dest)) {
            clog << " TCP client connecting to " << tcp_dest->dest_addr << ":" << tcp_dest->dest_port << endl;
            clog << "  max frames queued    " << tcp_dest->max_frames_queued << endl;
        }
        else {
            throw logic_error("EDI destination not implemented");
        }
    }
    if (interleaver_enabled()) {
        clog << " interleave     " << latency_frames * 24 << " ms" << endl;
    }
}


Sender::Sender(const configuration_t& conf) :
    m_conf(conf),
    edi_pft(m_conf)
{
    if (m_conf.verbose) {
        clog << "Setup EDI" << endl;
    }

    for (const auto& edi_dest : m_conf.destinations) {
        if (const auto udp_dest = dynamic_pointer_cast<edi::udp_destination_t>(edi_dest)) {
            auto udp_socket = std::make_shared<Socket::UDPSocket>(udp_dest->source_port);

            if (not udp_dest->source_addr.empty()) {
                udp_socket->setMulticastSource(udp_dest->source_addr.c_str());
                udp_socket->setMulticastTTL(udp_dest->ttl);
            }

            udp_sockets.emplace(udp_dest.get(), udp_socket);
        }
        else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_server_t>(edi_dest)) {
            auto dispatcher = make_shared<Socket::TCPDataDispatcher>(tcp_dest->max_frames_queued);
            dispatcher->start(tcp_dest->listen_port, "0.0.0.0");
            tcp_dispatchers.emplace(tcp_dest.get(), dispatcher);
        }
        else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_client_t>(edi_dest)) {
            auto tcp_socket = make_shared<Socket::TCPSocket>();
            tcp_socket->connect(tcp_dest->dest_addr, tcp_dest->dest_port);
            tcp_senders.emplace(tcp_dest.get(), tcp_socket);
        }
        else {
            throw logic_error("EDI destination not implemented");
        }
    }

    if (m_conf.interleaver_enabled()) {
        edi_interleaver.SetLatency(m_conf.latency_frames);
    }

    if (m_conf.dump) {
        edi_debug_file.open("./edi.debug");
    }

    if (m_conf.verbose) {
        clog << "EDI set up" << endl;
    }
}

void Sender::write(const TagPacket& tagpacket)
{
    // Assemble into one AF Packet
    edi::AFPacket af_packet = edi_afPacketiser.Assemble(tagpacket);

    if (m_conf.enable_pft) {
        // Apply PFT layer to AF Packet (Reed Solomon FEC and Fragmentation)
        vector<edi::PFTFragment> edi_fragments = edi_pft.Assemble(af_packet);

        if (m_conf.verbose) {
            fprintf(stderr, "EDI number of PFT fragment before interleaver %zu\n",
                    edi_fragments.size());
        }

        if (m_conf.interleaver_enabled()) {
            edi_fragments = edi_interleaver.Interleave(edi_fragments);
        }

        // Send over ethernet
        for (const auto& edi_frag : edi_fragments) {
            for (auto& dest : m_conf.destinations) {
                if (const auto& udp_dest = dynamic_pointer_cast<edi::udp_destination_t>(dest)) {
                    Socket::InetAddress addr;
                    addr.resolveUdpDestination(udp_dest->dest_addr, m_conf.dest_port);

                    udp_sockets.at(udp_dest.get())->send(edi_frag, addr);
                }
                else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_server_t>(dest)) {
                    tcp_dispatchers.at(tcp_dest.get())->write(edi_frag);
                }
                else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_client_t>(dest)) {
                    tcp_senders.at(tcp_dest.get())->sendall(edi_frag.data(), edi_frag.size());
                }
                else {
                    throw logic_error("EDI destination not implemented");
                }
            }

            if (m_conf.dump) {
                ostream_iterator<uint8_t> debug_iterator(edi_debug_file);
                copy(edi_frag.begin(), edi_frag.end(), debug_iterator);
            }
        }

        if (m_conf.verbose) {
            fprintf(stderr, "EDI number of PFT fragments %zu\n",
                    edi_fragments.size());
        }
    }
    else {
        // Send over ethernet
        for (auto& dest : m_conf.destinations) {
            if (const auto& udp_dest = dynamic_pointer_cast<edi::udp_destination_t>(dest)) {
                Socket::InetAddress addr;
                addr.resolveUdpDestination(udp_dest->dest_addr, m_conf.dest_port);

                udp_sockets.at(udp_dest.get())->send(af_packet, addr);
            }
            else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_server_t>(dest)) {
                tcp_dispatchers.at(tcp_dest.get())->write(af_packet);
            }
            else if (auto tcp_dest = dynamic_pointer_cast<edi::tcp_client_t>(dest)) {
                tcp_senders.at(tcp_dest.get())->sendall(af_packet.data(), af_packet.size());
            }
            else {
                throw logic_error("EDI destination not implemented");
            }
        }

        if (m_conf.dump) {
            ostream_iterator<uint8_t> debug_iterator(edi_debug_file);
            copy(af_packet.begin(), af_packet.end(), debug_iterator);
        }
    }
}

}
