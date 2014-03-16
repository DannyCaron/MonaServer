/*
Copyright 2014 Mona
mathieu.poux[a]gmail.com
jammetthomas[a]gmail.com

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License received along this program for more
details (or else see http://www.gnu.org/licenses/).

This file is a part of Mona.
*/

#include "ServerConnection.h"
#include "Mona/Util.h"
#include "Mona/Logs.h"


using namespace std;
using namespace Mona;


ServerConnection::ServerConnection(const SocketManager& manager, ServerHandler& handler, ServersHandler& serversHandler,const SocketAddress& targetAddress) : address(targetAddress), _size(0), _handler(handler), TCPClient(manager), _connected(false), _serversHandler(serversHandler), isTarget(true) {

}

ServerConnection::ServerConnection(const SocketAddress& peerAddress,SocketFile& file,const SocketManager& manager, ServerHandler& handler, ServersHandler& serversHandler) : address(peerAddress), _size(0), _handler(handler), TCPClient(peerAddress, file,manager), _connected(false), _serversHandler(serversHandler), isTarget(false) {
	sendPublicAddress();
}

ServerConnection::~ServerConnection() {
	close();
}

UInt16 ServerConnection::port(const string& protocol) {
	map<string, UInt16>::const_iterator it = _ports.find(protocol);
	if(it==_ports.end())
		return 0;
	return it->second;
}

void ServerConnection::sendPublicAddress() {
	ServerMessage message(manager().poolBuffers);
	BinaryWriter& writer = message.packet;
	writer.writeString(_handler.host());
	writer.write8(_handler.ports().size());
	map<string, UInt16>::const_iterator it0;
	for(it0=_handler.ports().begin();it0!=_handler.ports().end();++it0) {
		writer.writeString(it0->first);
		writer.write16(it0->second);
	}
	for(auto& it: *this) {
		writer.writeString(it.first);
		writer.writeString(it.second);
	}
	send("",message);
}

void ServerConnection::connect() {
	if(_connected)
		return;
	INFO("Attempt to join ", address.toString(), " server")
	Exception ex;
	bool success = false;
	EXCEPTION_TO_LOG(success=TCPClient::connect(ex, address),"ServerConnection")
	if (success)
		sendPublicAddress();
}

void ServerConnection::send(const string& handler,ServerMessage& message) {
	string handlerName(handler);
	if(handlerName.size()>255) {
		handlerName.resize(255);
		WARN("The server handler '",handlerName,"' truncated for 255 char (maximum acceptable size)")
	}

	// Search handler!
	UInt32 handlerRef = 0;
	bool   writeRef = false;
	if(!handlerName.empty()) {
		map<string, UInt32>::iterator it = _sendingRefs.lower_bound(handlerName);
		if(it!=_sendingRefs.end() && it->first==handlerName) {
			handlerRef = it->second;
			handlerName.clear();
			writeRef = true;
		} else {
			if(it!=_sendingRefs.begin())
				--it;
			handlerRef = _sendingRefs.size()+1;
			_sendingRefs.insert(it, pair<string, UInt32>(handlerName, handlerRef));
		}
	}

	UInt16 shift = handlerName.empty() ? Util::Get7BitValueSize(handlerRef) : handlerName.size();
	shift = 300-(shift+5);

	PacketWriter& packet = message.packet;

	UInt32 size = packet.size()-shift;

	BinaryWriter writer(packet, shift);

	writer.write32(size-4);
	writer.writeString8(handlerName);
	if(writeRef)
		writer.write7BitEncoded(handlerRef);
	else if(handlerName.empty())
		writer.write8(0);
	writer.next(size);

	DUMP_INTERN(writer.data() + 4, writer.size() - 4, "To ", address.toString()," server");
	Exception ex;
	EXCEPTION_TO_LOG(TCPClient::send(ex,writer.data(),writer.size()),"Server ",address.toString());
}


UInt32 ServerConnection::onReception(PoolBuffer& pBuffer) {
	if (_size == 0 && pBuffer->size() < 4)
		return pBuffer->size();

	PacketReader packet(pBuffer->data(), pBuffer->size());
	if(_size==0)
		_size = packet.read32();
	if (packet.available() < _size)
		return pBuffer->size();

	UInt32 rest = packet.available() - _size;
	packet.shrink(_size);
	
	DUMP_INTERN(packet.current(),packet.available(), "From ", address.toString(), " server");

	string handler;
	UInt8 handlerSize = packet.read8();
	if(handlerSize)
		_receivingRefs[_receivingRefs.size() + 1] = packet.readRaw(handlerSize, handler);
	else {
		UInt32 ref = packet.read7BitEncoded();
		if(ref>0) {
			map<UInt32, string>::const_iterator it = _receivingRefs.find(ref);
			if(it==_receivingRefs.end())
				ERROR("Impossible to find the ", ref, " handler reference for the server ", address.toString())
			else
				handler.assign(it->second);
		}
	}

	_size=0;
	if(handler.empty()) {
		packet.readString((string&)host);

		if(host.empty())
			((string&)host) = address.host().toString();
		UInt8 ports = packet.read8();
		string protocol;
		while(ports>0) {
			packet.readString(protocol);
			_ports[protocol] = packet.read16();
			--ports;
		}
		while(packet.available()) {
			string key,value;
			packet.readString(key);
			packet.readString(value);
			setString(key,value);
		}
		if(!_connected) {
			_connected=true;
			_serversHandler.connection(*this);
			NOTE("Connection etablished with ",address.toString()," server ")
		}
	} else
		_handler.message(*this,handler,packet);

	return rest;
}

void ServerConnection::onDisconnection(){
	_sendingRefs.clear();
	_receivingRefs.clear();
	if(_connected) {
		_connected=false;
		_serversHandler.disconnection(*this);
		if (_error.empty())
			NOTE("Disconnection from ", address.toString(), " server ")
		else
			ERROR("Disconnection from ", address.toString(), " server, ",_error)
		_ports.clear();
		((string&)host).clear();
	}
	_error.clear();
	if (!isTarget)
		delete this;
}
