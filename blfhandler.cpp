#include "blfhandler.h"
#include <QDebug>
#include <QFile>
#include <QString>
#include <QtEndian>

#define BLF_REMOTE_FLAG 0x80
#define BLF_CANFD64_REMOTE_FLAG 0x0010

BLFHandler::BLFHandler()
{

}

/*
 Written while peeking at source code here:
https://python-can.readthedocs.io/en/latest/_modules/can/io/blf.html
https://bitbucket.org/tobylorenz/vector_blf/

All the code actually below is freshly written but heavily based upon things seen in those
two source repos.
*/
bool BLFHandler::loadBLF(QString filename, QVector<CANFrame>* frames)
{
    BLF_OBJ_HEADER objHeader;
    QByteArray fileData;
    QByteArray uncompressedData;
    QByteArray junk;
    BLF_OBJECT obj;
    uint32_t pos;
    BLF_CAN_OBJ canObject;
    BLF_CAN_OBJ2 canObject2;
    BLF_CANFD_OBJ canFdObject;
    BLF_CANFD64_OBJ canFd64Object;

    QFile *inFile = new QFile(filename);

    if (!inFile->open(QIODevice::ReadOnly))
    {
        delete inFile;
        return false;
    }
    inFile->read((char *)&header, sizeof(header));
    if (qFromLittleEndian(header.sig) == 0x47474F4C)
    {
        qDebug() << "Proper BLF file header token";
    }
    else return false;

    qDebug() << "Header size" << header.headerSize;
    qDebug() << "File size" << header.fileSize;
    qDebug() << "Uncompressed size" << header.uncompressedFileSize;

    while (!inFile->atEnd())
    {
        qDebug() << "Position within file: " << inFile->pos();
        inFile->read((char *)&objHeader.base, sizeof(BLF_OBJ_HEADER_BASE));
        if (qFromLittleEndian(objHeader.base.sig) == 0x4A424F4C)
        {
            int readSize = objHeader.base.objSize - sizeof(BLF_OBJ_HEADER_BASE);
            qDebug() << "Proper object header token. Read Size: " << readSize;
            fileData = inFile->read(readSize);
            junk = inFile->read(readSize % 4); //file is padded so sizes must always end up on even multiple of 4
            //qDebug() << "Fudge bytes in readSize: " << (readSize % 4);

            switch (objHeader.base.objType)
            {
                case BLF_CONTAINER:
                qDebug() << "Object is a container.";
                memcpy(&objHeader.containerObj, fileData.constData(), sizeof(BLF_OBJ_HEADER_CONTAINER));
                fileData.remove(0, sizeof(BLF_OBJ_HEADER_CONTAINER));
                if (objHeader.containerObj.compressionMethod == BLF_CONT_NO_COMPRESSION)
                {
                    qDebug() << "Container is not compressed";
                    uncompressedData = fileData;
                }
                else if (objHeader.containerObj.compressionMethod == BLF_CONT_ZLIB_COMPRESSION)
                {
                    qDebug() << "Compressed container. Unpacking it.";
                    fileData.prepend(objHeader.containerObj.uncompressedSize & 0xFF);
                    fileData.prepend((objHeader.containerObj.uncompressedSize >> 8) & 0xFF);
                    fileData.prepend((objHeader.containerObj.uncompressedSize >> 16) & 0xFF);
                    fileData.prepend((objHeader.containerObj.uncompressedSize >> 24) & 0xFF);
                    uncompressedData += qUncompress(fileData);
                }
                else
                {
                    qDebug() << "Dunno what this is... " << objHeader.containerObj.compressionMethod;
                }
                qDebug() << "Uncompressed size: " << uncompressedData.count();
                qDebug() << "Currently loaded frames at this point: " << frames->count();
                pos = 0;
                //bool foundHeader = false;
                //first skip forward to find a header signature - usually not necessary
                while ( (int)(pos + sizeof(BLF_OBJ_HEADER)) < uncompressedData.count())
                {
                    int32_t *headerSig = (int32_t *)(uncompressedData.constData() + pos);
                    if (*headerSig == 0x4A424F4C) break;
                    pos += 4;
                }
                //then process all the objects
                while ( (int)(pos + sizeof(BLF_OBJ_HEADER)) < uncompressedData.count())
                {
                    memcpy(&obj.header.base, (uncompressedData.constData() + pos), sizeof(BLF_OBJ_HEADER_BASE));
                    memcpy(&obj.header.v1Obj, (uncompressedData.constData() + pos) + sizeof(BLF_OBJ_HEADER_BASE), sizeof(BLF_OBJ_HEADER_V1));
                    //if (obj.header.base.objType != 1)
                        //qDebug() << "Pos: " << pos << " Type: " << obj.header.base.objType << "Obj Size: " << obj.header.base.objSize;
                    if (qFromLittleEndian(objHeader.base.sig) == 0x4A424F4C)
                    {
                        fileData = uncompressedData.mid(pos + sizeof(BLF_OBJ_HEADER_BASE) + sizeof(BLF_OBJ_HEADER_V1), obj.header.base.objSize - sizeof(BLF_OBJ_HEADER_BASE) - sizeof(BLF_OBJ_HEADER_V1));
                        if (obj.header.base.objType == BLF_CAN_MSG)
                        {
                            memcpy(&canObject, fileData.constData(), sizeof(BLF_CAN_OBJ));
                            CANFrame frame;
                            frame.bus = canObject.channel;
                            frame.setExtendedFrameFormat((canObject.id & 0x80000000ull)?true:false);
                            frame.setFrameId(canObject.id & 0x1FFFFFFFull);
                            frame.isReceived = true;
                            QByteArray bytes(canObject.dlc, 0);

                            if (canObject.flags & BLF_REMOTE_FLAG) {
                                frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
                            } else {
                                frame.setFrameType(QCanBusFrame::DataFrame);
                                for (int i = 0; i < canObject.dlc; i++) bytes[i] = canObject.data[i];
                            }
                            frame.setPayload(bytes);

                            float timeStampDivisor = obj.header.v1Obj.flags == 1 ? 0.2 : 1000; //TODO: implement for v2 obj
                            frame.setTimeStamp(QCanBusFrame::TimeStamp(0, obj.header.v1Obj.objTimeStamp / timeStampDivisor));
                            frames->append(frame);
                        }
                        else if (obj.header.base.objType == BLF_CAN_MSG2)
                        {
                            memcpy(&canObject2, fileData.constData(), sizeof(BLF_CAN_OBJ2));
                            CANFrame frame;
                            frame.bus = canObject2.channel;
                            frame.setExtendedFrameFormat((canObject2.id & 0x80000000ull)?true:false);
                            frame.setFrameId(canObject2.id & 0x1FFFFFFFull);
                            frame.isReceived = true;
                            QByteArray bytes(canObject2.dlc, 0);

                            if (canObject2.flags & BLF_REMOTE_FLAG) {
                                frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
                            } else {
                                frame.setFrameType(QCanBusFrame::DataFrame);
                                for (int i = 0; i < canObject2.dlc; i++) bytes[i] = canObject2.data[i];
                            }
                            frame.setPayload(bytes);

                            float timeStampDivisor = obj.header.v1Obj.flags == 1 ? 0.2 : 1000; //TODO: implement for v2 obj
                            frame.setTimeStamp(QCanBusFrame::TimeStamp(0, obj.header.v1Obj.objTimeStamp / timeStampDivisor));
                            frames->append(frame);
                        }
                        else if (obj.header.base.objType == BLF_CAN_FD_MSG) {
                            qDebug() << "Got BLF_CAN_FD_MSG";
                            memcpy(&canFdObject, fileData.constData(), sizeof(BLF_CANFD_OBJ));
                            CANFrame frame;
                            frame.bus = canFdObject.channel;
                            frame.setExtendedFrameFormat((canFdObject.id & 0x80000000ull)?true:false);
                            frame.setFrameId(canFdObject.id & 0x1FFFFFFFull);
                            frame.isReceived = true;
                            QByteArray bytes(canFdObject.dlc, 0);

                            if (canFdObject.flags & BLF_REMOTE_FLAG) {
                                frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
                            } else {
                                frame.setFrameType(QCanBusFrame::DataFrame);
                                for (int i = 0; i < canFdObject.dlc; i++) bytes[i] = canFdObject.data[i];
                            }
                            frame.setPayload(bytes);

                            float timeStampDivisor = obj.header.v1Obj.flags == 1 ? 0.2 : 1000; //TODO: implement for v2 obj
                            frame.setTimeStamp(QCanBusFrame::TimeStamp(0, obj.header.v1Obj.objTimeStamp / timeStampDivisor));
                            frames->append(frame);
                        }
                        else if(obj.header.base.objType == BLF_CAN_FD_MSG64) {

                            //qDebug() << "Got BLF_CAN_FD_MSG64. Pos: " << pos << ", size: " << obj.header.base.objSize;
                            memcpy(&canFd64Object, fileData.constData(), sizeof(BLF_CANFD64_OBJ));
                            CANFrame frame;
                            frame.bus = canFd64Object.channel;
                            frame.setExtendedFrameFormat((canFd64Object.id & 0x80000000ull)?true:false);
                            frame.setFrameId(canFd64Object.id & 0x1FFFFFFFull);
                            frame.isReceived = true;


                            uint8_t msgDlc = canFd64Object.getDlc();
                            QByteArray bytes(msgDlc, 0);
                            if (canFd64Object.fdFlags & BLF_CANFD64_REMOTE_FLAG) {
                                frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
                            } else {
                                frame.setFrameType(QCanBusFrame::DataFrame);
                                bytes = fileData.mid(sizeof(BLF_CANFD64_OBJ), msgDlc);
                            }
                            frame.setPayload(bytes);

                            float timeStampDivisor = obj.header.v1Obj.flags == 1 ? 0.2 : 1000; //TODO: implement for v2 obj
                            frame.setTimeStamp(QCanBusFrame::TimeStamp(0, obj.header.v1Obj.objTimeStamp / timeStampDivisor));
                            frames->append(frame);
                        }
                        else if (obj.header.base.objType == BLF_APP_TEXT) {
                            qDebug() << "Got BLF_APP_TEXT";
                        }
                        else
                        {
                            if (obj.header.base.objType > 0xFFFF)
                                return false;
                        }

                        pos += obj.header.base.objSize;
                        uint32_t newPos = pos;
                        while (((newPos - pos) <= (obj.header.base.objSize % 4)) && uncompressedData.mid(newPos, 4) != "LOBJ") {
                            newPos++;
                        }
                        pos = newPos;
                    }
                    else
                    {
                        qDebug() << "Unexpected object header signature, aborting";
                        return false;
                    }
                }
                uncompressedData.remove(0, pos);
                qDebug() << "After removing used data uncompressedData is now this big: " << uncompressedData.count();

                break;
            }
        }
        else return false;
    }
    return true;
}

bool BLFHandler::saveBLF(QString filename, QVector<CANFrame> *frames)
{
    Q_UNUSED(filename)
    Q_UNUSED(frames)
    return false;
}
