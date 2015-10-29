//This program is free software: you can redistribute it and/or modify
//it under the terms of the GNU Lesser General Public License as published by
//the Free Software Foundation, either version 3 of the License, or
//(at your option) any later version.

//This program is distributed in the hope that it will be useful,
//but WITHOUT ANY WARRANTY; without even the implied warranty of
//MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//GNU Lesser General Public License for more details.

// Copyright (c) Petr Bena 2015

#include <QTcpSocket>
#include <QSslSocket>
#include <QDataStream>
#include "gp_exception.h"
#include "gp.h"

using namespace libgp;

GP::GP(QTcpSocket *tcp_socket)
{
    this->socket = tcp_socket;
    this->sentBytes = 0;
    this->recvBytes = 0;
    // We don't want to receive single packet bigger than 800kb
    this->MaxIncomingCacheSize = 800000;
    this->incomingPacketSize = 0;
}

GP::~GP()
{
    delete this->socket;
}

bool GP::IsConnected() const
{
    if (!this->socket)
        return false;
    return this->socket->isOpen();
}

void GP::OnPing()
{

}

void GP::OnPingSend()
{

}

void GP::OnError(QAbstractSocket::SocketError er)
{
    qDebug() << "Socket error: " + this->socket->errorString();
}

void GP::OnReceive()
{
    this->processIncoming(this->socket->readAll());
}

void GP::OnConnected()
{

}

void GP::OnDisconnect()
{
    emit this->Event_Disconnected();
}

void GP::OnIncomingCommand(QString text, QHash<QString, QVariant> parameters)
{
    emit this->Event_IncomingCommand(text, parameters);
}

void GP::processPacket(QHash<QString, QVariant> pack)
{
    if (!pack.contains("type"))
        throw new GP_Exception("Broken packet");

    emit this->Event_Incoming(pack);

    int type = pack["type"].toInt();
    switch (type)
    {
        case GP_TYPE_SYSTEM:
        {
            if (!pack.contains("text"))
                throw new GP_Exception("Broken packet");
            QHash<QString, QVariant> parameters;
            if (pack.contains("parameters"))
                parameters = pack["parameters"].toHash();
            this->OnIncomingCommand(pack["text"].toString(), parameters);
        }
            break;
    }
}

void GP::processIncoming(QByteArray data)
{
    this->recvBytes += static_cast<unsigned long long>(data.size());
    if (this->incomingPacketSize)
    {
        // we are already receiving a packet
        int remaining_packet_data = this->incomingPacketSize - this->incomingCache.size();
        if (remaining_packet_data < 0)
            throw new GP_Exception("Negative packet size");
        if (data.size() == remaining_packet_data)
        {
            // this is extremely rare
            // the packet we received is exactly the remaining part of a block of data we are receiving
            this->incomingCache.append(data);
            this->processPacket(this->packetFromIncomingCache());
        } else if (data.size() < remaining_packet_data)
        {
            // just append and skip
            this->incomingCache.append(data);
        } else if (data.size() > remaining_packet_data)
        {
            // this is most common and yet most annoying situation, the data we received are bigger than
            // packet, so we need to cut the remaining part and process
            QByteArray remaining_part = data.mid(0, remaining_packet_data);
            this->incomingCache.append(remaining_part);
            this->processPacket(this->packetFromIncomingCache());
            data = data.mid(remaining_packet_data);
            this->processIncoming(data);
        }
    } else
    {
        int current_data_size = this->incomingCache.size() + data.size();
        int remaining_header_size = GP_HEADER_SIZE - this->incomingCache.size();
        // we just started receiving a new packet
        if (current_data_size < GP_HEADER_SIZE)
        {
            // great, the data is so small they don't even contain the header yet, let's cache this and wait for later
            this->incomingCache.append(data);
        } else if (current_data_size == GP_HEADER_SIZE)
        {
            // we received just a header
            this->incomingCache.append(data);
            this->processHeader(this->incomingCache);
        } else if (current_data_size > GP_HEADER_SIZE)
        {
            // we received a header and some block of data
            // first grab the remaining bytes that belong to header and cut them out
            QByteArray remaining_header_data = data.mid(0, remaining_header_size);
            data = data.mid(remaining_header_size);
            // now construct the header from cache and newly retrieved bytes
            QByteArray header = this->incomingCache + remaining_header_data;
            // now we have the header and remaining data, so continue processing
            this->processHeader(header);
            this->processIncoming(data);
        }
    }
}

static QByteArray ToArray(QHash<QString, QVariant> data)
{
    QByteArray result;
    QDataStream stream(&result, QIODevice::ReadWrite);
    GP_INIT_DS(stream);
    stream << data;
    return result;
}

static QByteArray ToArray(int number)
{
    QByteArray result;
    QDataStream stream(&result, QIODevice::ReadWrite);
    GP_INIT_DS(stream);
    stream << qint64(number);
    return result;
}

QHash<QString, QVariant> GP::packetFromIncomingCache()
{
    QDataStream stream(&this->incomingCache, QIODevice::ReadWrite);
    GP_INIT_DS(stream);
    QHash<QString, QVariant> data;
    stream >> data;
    this->incomingPacketSize = 0;
    this->incomingCache.clear();
    return data;
}

void GP::processHeader(QByteArray data)
{
    qint64 header;
    QDataStream stream(&data, QIODevice::ReadWrite);
    GP_INIT_DS(stream);
    stream >> header;
    if (header < 0)
        throw new GP_Exception("Negative header size");
    this->incomingCache.clear();
    this->incomingPacketSize = header;
}

bool GP::SendPacket(QHash<QString, QVariant> packet)
{
    if (!this->socket)
        return false;
    // Current format of every packet is extremely simple
    // First GP_HEADER_SIZE bytes are the size of packet
    // Following bytes are the packet itself
    QByteArray result = ToArray(packet);
    QByteArray header = ToArray(result.size());
    if (header.size() != GP_HEADER_SIZE)
        throw new GP_Exception("Invalid header size: " + QString::number(header.size()));
    result.prepend(ToArray(result.size()));
    // We must lock the connection here to prevent multiple threads from writing into same socket thus writing borked data
    // into it
    this->mutex.lock();
    this->sentBytes += static_cast<unsigned long long>(result.size());
    this->socket->write(result);
    this->socket->flush();
    this->mutex.unlock();
    return true;
}

void GP::SendProtocolCommand(QString command)
{
    this->SendProtocolCommand(command, QHash<QString, QVariant>());
}

void GP::SendProtocolCommand(QString command, QHash<QString, QVariant> parameters)
{
    QHash<QString, QVariant> pack;
    pack.insert("type", QVariant(GP_TYPE_SYSTEM));
    pack.insert("text", QVariant(command));
    pack.insert("parameters", QVariant(parameters));
    this->SendPacket(pack);
}

void GP::ResolveSignals()
{
    if (!this->socket)
        throw new GP_Exception("this->socket");
    connect(this->socket, SIGNAL(readyRead()), this, SLOT(OnReceive()));
    connect(this->socket, SIGNAL(disconnected()), this, SLOT(OnDisconnect()));
    connect(this->socket, SIGNAL(error(QAbstractSocket::SocketError)), this, SLOT(OnError(QAbstractSocket::SocketError)));
}

void GP::Disconnect()
{
    if (!this->socket)
        return;
    if (this->socket->isOpen())
        this->socket->close();
    this->socket->deleteLater();
    this->socket = NULL;
}

unsigned long long GP::GetBytesSent()
{
    return this->sentBytes;
}

unsigned long long GP::GetBytesRcvd()
{
    return this->recvBytes;
}

int GP::GetVersion()
{
    return GP_VERSION;
}


