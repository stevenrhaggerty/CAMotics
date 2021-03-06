/******************************************************************************\

    CAMotics is an Open-Source simulation and CAM software.
    Copyright (C) 2011-2017 Joseph Coffland <joseph@cauldrondevelopment.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

\******************************************************************************/

#include "BBCtrlAPI.h"

#include "QtWin.h"

#include <camotics/view/View.h>
#include <camotics/view/ToolPathView.h>

#include <cbang/os/SystemUtilities.h>
#include <cbang/log/Logger.h>
#include <cbang/time/Time.h>
#include <cbang/io/StringInputSource.h>
#include <cbang/util/DefaultCatch.h>

#include <QHttpMultiPart>
#include <QNetworkAccessManager>
#include <QNetworkReply>

using namespace CAMotics;
using namespace cb;
using namespace std;


BBCtrlAPI::BBCtrlAPI(QtWin *parent) :
  parent(parent), netManager(new QNetworkAccessManager(this)), active(false),
  _connected(false) {
  connect(&webSocket, SIGNAL(error(QAbstractSocket::SocketError)), this,
          SLOT(onError(QAbstractSocket::SocketError)));
  connect(&webSocket, SIGNAL(connected()), this, SLOT(onConnected()));
  connect(&webSocket, SIGNAL(disconnected()), this, SLOT(onDisconnected()));
  connect(&webSocket, SIGNAL(textMessageReceived(QString)), this,
          SLOT(onTextMessageReceived(QString)));

  updateTimer.setSingleShot(false);
  reconnectTimer.setSingleShot(true);

  connect(&updateTimer, SIGNAL(timeout()), this, SLOT(onUpdate()));
  connect(&reconnectTimer, SIGNAL(timeout()), this, SLOT(onReconnect()));
}


void BBCtrlAPI::connectCNC(const QString &address) {
  disconnectCNC();

  url = QString("ws://") + address + QString("/websocket");
  reconnect();

  active = true;
}


void BBCtrlAPI::disconnectCNC() {
  active = false;
  webSocket.close();
  updateTimer.stop();
  reconnectTimer.stop();
}


void BBCtrlAPI::reconnect() {
  updateTimer.stop();
  uint64_t delta = (Time::now() - lastMessage) * 1000;
  reconnectTimer.start(lastMessage ? std::min(delta, (uint64_t)4500) : 1);
}


void BBCtrlAPI::uploadGCode(const char *data, unsigned length) {
  LOG_INFO(1, "Uploading GCode '" << filename << "'");

  // Create multi-part MIME encoded message
  QHttpMultiPart *multiPart = new QHttpMultiPart(QHttpMultiPart::FormDataType);

  QHttpPart part;
  part.setHeader(QNetworkRequest::ContentTypeHeader, QVariant("text/plain"));

  part.setHeader(QNetworkRequest::ContentDispositionHeader,
                 QString("form-data; name=\"gcode\"; filename=\"%1\"")
                 .arg(QString::fromUtf8(filename.c_str())));
  part.setBody(QByteArray::fromRawData(data, length));

  multiPart->append(part);

  // Upload
  QUrl url = QString("http://") + this->url.host() + QString("/api/file");
  QNetworkRequest request(url);

  if (!netManager) netManager = new QNetworkAccessManager(this);

  QNetworkReply *reply = netManager->put(request, multiPart);
  multiPart->setParent(reply); // delete the multiPart with the reply
  // here connect signals etc.
}


void BBCtrlAPI::onError(QAbstractSocket::SocketError error) {
  LOG_WARNING("CNC: " << webSocket.errorString().toUtf8().data());
}


void BBCtrlAPI::onConnected() {
  LOG_INFO(1, "CNC connected");
  reconnectTimer.stop();
  updateTimer.start(1000);
  lastMessage = Time::now();

  if (!_connected) {
    _connected = true;
    emit connected();
  }
}


void BBCtrlAPI::onDisconnected() {
  LOG_INFO(1, "CNC disconnected");

  if (_connected) {
    _connected = false;
    emit disconnected();
  }

  if (active) reconnect();
}


void BBCtrlAPI::onTextMessageReceived(const QString &message) {
  string data = message.toUtf8().data();
  LOG_DEBUG(1, "CNC received: " << data);

  try {
    JSON::ValuePtr json = JSON::Reader::parse(StringInputSource(data));

    lastMessage = Time::now();

    bool updatePosition = false;

    for (unsigned i = 0; i < json->size(); i++) {
      string key = json->keyAt(i);
      vars.insert(key, json->get(key));

      if (key == "xp" || key == "yp" || key == "zp" ||
          key == "offset_x" || key == "offset_y" || key == "offset_z")
        updatePosition = true;
    }

    if (updatePosition) {
      uint32_t line = vars.getS32("ln", 0);
      Vector3D position(vars.getNumber("xp", 0),
                        vars.getNumber("yp", 0),
                        vars.getNumber("zp", 0));
      Vector3D offset(vars.getNumber("offset_x", 0),
                      vars.getNumber("offset_y", 0),
                      vars.getNumber("offset_z", 0));

      parent->getView()->path->setByRemote(position + offset, line);
      parent->redraw();
    }
  } CATCH_ERROR;
}


void BBCtrlAPI::onUpdate() {
  if (lastMessage + 5 < Time::now()) {
    LOG_WARNING("CNC timed out");
    reconnect();
  }
}


void BBCtrlAPI::onReconnect() {
  if (!active) return;

  LOG_INFO(1, "Connecting to " << url.toString().toUtf8().data());

  if (webSocket.isValid()) webSocket.close();
  webSocket.open(url);
}
