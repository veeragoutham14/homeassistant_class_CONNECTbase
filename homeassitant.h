#ifndef HOMEASSISTANT_H
#define HOMEASSISTANT_H

#include <QObject>
#include <QString>
#include <QScopedPointer>
#include <QMap>
#include <QTimer>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QtWebSockets/QWebSocketServer>
#include <QSet>

namespace QMdnsEngine{
class Server;
class Hostname;
class Provider;
class Service;
}

class QWebSocket;



class HomeAssistant : public QObject
{
	Q_OBJECT
public:
	explicit HomeAssistant(QObject *parent = nullptr);
	~HomeAssistant() override;
	//mdns part start
	void start_mdns(const QString& serviceType,
			   const QString& instanceName,
			   quint16& port,
			   const QMap<QByteArray, QByteArray>& txt);

	void stop_mdns();
	void republish_mdns();
	//mdns part ends here

	//websocketserver part starts
	bool start_webSocketServer(quint16 port = 8090); // Start listening on 0.0.0.0:port (defaults to 8090)
	void stop_webSocketServer();

	//Send Json to all connected clients
	void broadcastText(const QString& text);
	void broadcastJson(const QJsonObject& obj);

	int clientCount() const {return w_clients.size();}
	bool isListening() const {return w_server && w_server->isListening();}
	quint16 listeningPort() const {return w_listenport;}

	//for flattener
	// Build a flat schema from nested status (pure function)
	static QJsonObject makeHaFlat(const QJsonObject& status);
	// Broadcast a pre-flattened object with throttling + de-dupe
	void broadcastHaFlat(const QJsonObject& flat, bool force = false);

private:
	//mdns part starts
	QScopedPointer<QMdnsEngine::Server> mdns_server;
	QScopedPointer<QMdnsEngine::Hostname> mdns_hostname;
	QScopedPointer<QMdnsEngine::Provider> mdns_provider;
	QScopedPointer<QMdnsEngine::Service> mdns_service;

	QByteArray mdns_serviceType;
	QByteArray mdns_instanceName;
	quint16 mdns_port = 0;
	QMap<QByteArray, QByteArray> mdns_txt;
	//mdns part ends

	//websocket part starts
	std::unique_ptr<QWebSocketServer> w_server;
	QSet<QWebSocket*> w_clients;
	quint16 w_listenport = 0;
	//websocket part ends here

	// state for throttle/de-dupe
	QJsonObject  m_lastFlat;
	QElapsedTimer m_lastSend;

signals:
	void messageReceived(const QString& message);
	void clientCountChanged(int count);

private slots:
	void onNewConnection();
	void onTextMessageReceived(const QString& message);
	void onSocketDisconnected();
};

#endif // HOMEASSISTANT_H
