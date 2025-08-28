#include "homeassistant.h"

// qmdnsengine
#include <qmdnsengine/server.h>
#include <qmdnsengine/hostname.h>
#include <qmdnsengine/provider.h>
#include <qmdnsengine/service.h>

#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QtWebSockets/QWebSocket>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <QMetaObject>
HomeAssistant::HomeAssistant(QObject *parent)
	: QObject{parent}
{
	// mdns core stack
	mdns_server.reset(new QMdnsEngine::Server(this));
	mdns_hostname.reset(new QMdnsEngine::Hostname(mdns_server.data(), this));
	mdns_provider.reset(new QMdnsEngine::Provider(mdns_server.data(), mdns_hostname.data(), this));
}

HomeAssistant::~HomeAssistant()
{
	stop_mdns();
	stop_webSocketServer();
}

/* ------------------- mDNS ------------------- */

void HomeAssistant::start_mdns(const QString& serviceType,
							   const QString& instanceName,
							   quint16& port,
							   const QMap<QByteArray, QByteArray>& txt)
{
	// DO NOT touch the WebSocket server here.

	// Make sure the mdns plumbing exists
	if (!mdns_server)
		mdns_server.reset(new QMdnsEngine::Server(this));
	if (!mdns_hostname)
		mdns_hostname.reset(new QMdnsEngine::Hostname(mdns_server.data(), this));
	if (!mdns_provider)
		mdns_provider.reset(new QMdnsEngine::Provider(mdns_server.data(), mdns_hostname.data(), this));

	// Cache params for republish
	mdns_serviceType = serviceType.toUtf8();
	mdns_instanceName = instanceName.toUtf8();
	mdns_port = port;
	mdns_txt = txt;

	// Build and publish service record
	mdns_service.reset(new QMdnsEngine::Service);
	mdns_service->setType(mdns_serviceType);
	mdns_service->setName(mdns_instanceName);
	mdns_service->setPort(static_cast<uint16_t>(mdns_port));

	// TXT attributes
	for (auto it = mdns_txt.begin(); it != mdns_txt.end(); ++it)
		mdns_service->addAttribute(it.key(), it.value());

	mdns_provider->update(*mdns_service);
	qInfo() << "[MdnsService] Published" << serviceType
			<< "as" << instanceName
			<< "on port" << mdns_port;
}

void HomeAssistant::stop_mdns()
{
	// Unpublish current service, but keep the core stack for quick restart
	if (mdns_provider && mdns_service) {
		qInfo() << "[MdnsService] Unpublished"
				<< QString::fromUtf8(mdns_serviceType)
				<< "instance" << QString::fromUtf8(mdns_instanceName);
	}
	mdns_service.reset(nullptr);

	// Keep server/hostname/provider around; we may republish soon
	// (If you really need to fully drop them, uncomment below)
	// mdns_provider.reset(nullptr);
	// mdns_hostname.reset(nullptr);
	// mdns_server.reset(nullptr);

	// Clear cached values (optional)
	mdns_port = 0;
	mdns_serviceType.clear();
	mdns_instanceName.clear();
	mdns_txt.clear();

	qInfo() << "[MdnsService] stopped";
}

void HomeAssistant::republish_mdns()
{
	// DO NOT touch the WebSocket server here.
	// Prefer a light update; only rebuild if service was cleared.

	if (mdns_provider && mdns_service) {
		mdns_provider->update(*mdns_service);
		qInfo() << "[MdnsService] Re-announced"
				<< QString::fromUtf8(mdns_serviceType)
				<< "instance" << QString::fromUtf8(mdns_instanceName);
		return;
	}

	if (mdns_serviceType.isEmpty() || mdns_instanceName.isEmpty() || mdns_port == 0) {
		qWarning() << "[MdnsService] republish() skipped; no cached parameters.";
		return;
	}

	// Rebuild service object from cached params
	mdns_service.reset(new QMdnsEngine::Service);
	mdns_service->setType(mdns_serviceType);
	mdns_service->setName(mdns_instanceName);
	mdns_service->setPort(static_cast<uint16_t>(mdns_port));
	for (auto it = mdns_txt.begin(); it != mdns_txt.end(); ++it)
		mdns_service->addAttribute(it.key(), it.value());

	if (!mdns_provider) {
		// Recreate provider if it was cleared
		if (!mdns_server)
			mdns_server.reset(new QMdnsEngine::Server(this));
		if (!mdns_hostname)
			mdns_hostname.reset(new QMdnsEngine::Hostname(mdns_server.data(), this));
		mdns_provider.reset(new QMdnsEngine::Provider(mdns_server.data(), mdns_hostname.data(), this));
	}

	mdns_provider->update(*mdns_service);
	qInfo() << "[MdnsService] Rebuilt and announced"
			<< QString::fromUtf8(mdns_serviceType)
			<< "instance" << QString::fromUtf8(mdns_instanceName)
			<< "on port" << mdns_port;
}

/* ------------------- WebSocket ------------------- */

bool HomeAssistant::start_webSocketServer(quint16 port)
{

	if (w_server && w_server->isListening()) {
		qInfo() << "[WebSocketService] already listening on port" << w_listenport;
		return true;
	}

	w_server = std::make_unique<QWebSocketServer>(
		QStringLiteral("CONNECTbase WS"),
		QWebSocketServer::NonSecureMode,
		this
	);

	if (!w_server->listen(QHostAddress::AnyIPv4, port)) {
		qWarning() << "[WebSocketService] listen failed on port" << port
				   << "error:" << w_server->errorString();
		w_server.reset();
		w_listenport = 0;
		return false;
	}

	w_listenport = port;

	connect(w_server.get(), &QWebSocketServer::newConnection,
			this, &HomeAssistant::onNewConnection);

	qInfo() << "[WebSocketService] listening on" << QHostAddress(QHostAddress::AnyIPv4).toString()
			<< ":" << port;
	return true;
}

void HomeAssistant::stop_webSocketServer()
{
	// Close and delete client sockets
	for (QWebSocket* sock : std::as_const(w_clients)) {
		disconnect(sock, nullptr, this, nullptr);
		sock->close();
		sock->deleteLater();
	}
	const auto prevCount = w_clients.size();
	w_clients.clear();
	if (prevCount)
		emit clientCountChanged(0);

	if (w_server) {
		if (w_server->isListening())
			w_server->close();
		w_server.reset();
	}
	w_listenport = 0;
	qInfo() << "[WebSocketService] stopped";
}

void HomeAssistant::onNewConnection()
{
	if (!w_server) return;

	QWebSocket* socket = w_server->nextPendingConnection();
	if (!socket) return;

	w_clients.insert(socket);

	connect(socket, &QWebSocket::textMessageReceived,
			this, &HomeAssistant::onTextMessageReceived);

	connect(socket, &QWebSocket::disconnected,
			this, &HomeAssistant::onSocketDisconnected);

#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
	connect(socket, &QWebSocket::errorOccurred, this, [socket](auto){
		qWarning() << "[WebSocketService] socket error:"
				   << socket->error() << socket->errorString();
	});
#else
	connect(socket,
			static_cast<void(QWebSocket::*)(QAbstractSocket::SocketError)>(&QWebSocket::error),
			this, [socket](QAbstractSocket::SocketError){
				qWarning() << "[WebSocketService] socket error:"
						   << socket->error() << socket->errorString();
			});
#endif

	// Keepalive ping every 15s to survive short stalls
	auto* ka = new QTimer(socket);
	ka->setInterval(15000);
	connect(ka, &QTimer::timeout, socket, [socket]{
		socket->ping();
	});
	connect(socket, &QWebSocket::pong, socket, [](quint64 /*elapsed*/, const QByteArray& /*payload*/){
		// could log/debug if needed
	});
	ka->start();

	emit clientCountChanged(w_clients.size());

	qInfo() << "[WebSocketService] client connected from"
			<< socket->peerAddress().toString() << ":" << socket->peerPort();
}

void HomeAssistant::onTextMessageReceived(const QString &message)
{
	// Forward upstream; parsing (JSON) happens in WorkItem
	emit messageReceived(message);
}

void HomeAssistant::onSocketDisconnected()
{
	QWebSocket* socket = qobject_cast<QWebSocket*>(sender());
	if (!socket) return;

	// Log close diagnostics before we delete the socket
	qInfo() << "[WebSocketService] client disconnected;"
			<< "remaining" << (w_clients.size() ? w_clients.size()-1 : 0)
			<< "code:"   << socket->closeCode()
			<< "reason:" << socket->closeReason()
			<< "error:"  << socket->error() << socket->errorString();

	w_clients.remove(socket);
	socket->deleteLater();

	emit clientCountChanged(w_clients.size());
}

/* ------------------- Broadcast helpers ------------------- */

void HomeAssistant::broadcastText(const QString &text)
{
	// If called from another thread (e.g. TuProtocol worker), hop to ours
	if (QThread::currentThread() != this->thread()) {
		const QString copy = text;
		QMetaObject::invokeMethod(this, [this, copy]{
			this->broadcastText(copy);
		}, Qt::QueuedConnection);
		return;
	}

	for (QWebSocket* s : std::as_const(w_clients)) {
		s->sendTextMessage(text);
		qDebug() << "Text sent to home assistant" << text;
	}
}


void HomeAssistant::broadcastJson(const QJsonObject& obj)
{
	const QString payload = QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
	broadcastText(payload);
}

/* ------------------- HA flatten + throttled broadcast ------------------- */

QJsonObject HomeAssistant::makeHaFlat(const QJsonObject& status)
{
	QJsonObject flat;

	auto put = [&](const QString& k, const QJsonValue& v) {
		if (v.isUndefined() || v.isNull()) {
			flat.insert(k, QString(""));
		} else {
			flat.insert(k, v);
		}
	};

	// Basics
	put("device_id", status.value("deviceId").toString(status.value("id").toString()));
	put("id",        status.value("id").toString());
	put("is_online", status.value("isOnline").toBool());
	put("is_in_use", status.value("isInUse").toBool());
	put("has_info",  status.value("hasInfo"));

	// Hygiene
	const auto hs = status.value("hygieneState").toObject();
	for (auto it = hs.begin(); it != hs.end(); ++it)
		put(QStringLiteral("hygiene_%1").arg(it.key()), it.value());

	// Notifications
	const auto notifs = status.value("notifications").toArray();
	put("notifications_count", int(notifs.size()));
	if (!notifs.isEmpty() && notifs.first().isObject()) {
		const auto n0 = notifs.first().toObject();
		put("notification_category",    n0.value("category"));
		put("notification_description", n0.value("description"));
		put("notification_errorNumber", n0.value("errorNumber"));
		put("notification_id",          n0.value("id"));
		put("notification_text",        n0.value("text"));
	} else {
		put("notification_category",    "");
		put("notification_description", "");
		put("notification_errorNumber", "");
		put("notification_id",          "");
		put("notification_text",        "");
	}
	for (int i = 0; i < notifs.size(); ++i) {
		const auto n = notifs.at(i).toObject();
		for (auto it = n.begin(); it != n.end(); ++it)
			put(QString("notifications_%1_%2").arg(i).arg(it.key()), it.value());
		}

	// Critical errors
	const auto crit = status.value("criticalErrors").toArray();
	put("critical_errors_count", int(crit.size()));
	for (int i = 0; i < crit.size(); ++i) {
		const auto c = crit.at(i).toObject();
		for (auto it = c.begin(); it != c.end(); ++it)
			put(QString("critical_%1_%2").arg(i).arg(it.key()), it.value());
	}

	// Other notifications
	const auto other = status.value("otherNotifications").toArray();
	put("other_notifications_count", int(other.size()));
	for (int i = 0; i < other.size(); ++i) {
		const auto o = other.at(i).toObject();
		for (auto it = o.begin(); it != o.end(); ++it)
			put(QString("other_notifications_%1_%2").arg(i).arg(it.key()), it.value());
	}

	// Additional status
	const auto add = status.value("additionalStatusFields").toArray();
	put("additional_status_fields_count", int(add.size()));
	for (int i = 0; i < add.size(); ++i) {
		const auto v = add.at(i);
		if (v.isObject()) {
			const auto o = v.toObject();
			for (auto it = o.begin(); it != o.end(); ++it)
				put(QString("additional_%1_%2").arg(i).arg(it.key()), it.value());
		} else if (v.isArray()) {
			put(QString("additional_%1").arg(i),
				QString::fromUtf8(QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact)));
		} else {
			put(QString("additional_%1").arg(i), v);
		}
	}

	return flat;
}

void HomeAssistant::broadcastHaFlat(const QJsonObject& jsonobj, bool force)
{
	// Ensure all state (throttle/de-dupe) and socket I/O run on our thread
	if (QThread::currentThread() != this->thread()) {
		const QJsonObject copy = jsonobj; // detach
		QMetaObject::invokeMethod(this, [this, copy, force]{
			this->broadcastHaFlat(copy, force);
		}, Qt::QueuedConnection);
		return;
	}

	QJsonObject flat = makeHaFlat(jsonobj);

	if (!force) {
		if (!m_lastFlat.isEmpty() && flat == m_lastFlat) return;
		if (m_lastSend.isValid() && m_lastSend.elapsed() < 150) return;
	}

	if (!m_lastSend.isValid()) m_lastSend.start();
	else m_lastSend.restart();

	broadcastJson(flat);
	m_lastFlat = flat;

	qDebug() << "[HomeAssistant] flat broadcast:"
			 << QString::fromUtf8(QJsonDocument(flat).toJson(QJsonDocument::Compact));
}
