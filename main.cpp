#include <array>
#include <boost/asio.hpp>
#include <boost/asio/streambuf.hpp>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>
#include <sys/wait.h>
#include <signal.h>
#include <boost/bind.hpp>
#include <boost/system/error_code.hpp>
#include <fstream> //read file

using namespace std;
using namespace boost::asio;

#define dataBufferSize 65536
#define socksReceiveBufferSize 128

void sig_handler(int sig) {
	if (sig == SIGCHLD)
		while (waitpid(-1, NULL, WNOHANG) > 0);
	signal(sig, sig_handler);
}

class SocksServerMainSocket {
private:
	io_service ioservice;
	ip::tcp::acceptor acceptor;
	ip::tcp::socket forClientSocket, forRemoteSocket;
	boost::asio::ip::tcp::endpoint forClientEndpoint, forRemoteEndpoint;
	boost::system::error_code ec;
	char sourceIP[16], destinationIP[16], data[dataBufferSize];	// in IPv4
	unsigned int sourcePort, destinationPort, mode;
	unsigned char socksReceiveBuffer[socksReceiveBufferSize];

public:
	SocksServerMainSocket(unsigned short port) :
		acceptor(ioservice, ip::tcp::endpoint(ip::tcp::v4(), port), true),
		forClientSocket(ioservice),
		forRemoteSocket(ioservice) {
		boost::asio::ip::tcp::resolver resolver(ioservice);
		resolver.resolve({ ip::tcp::v4(), to_string(port) });
		acceptor.listen(boost::asio::socket_base::max_connections);
	}

	bool accept() {
		acceptor.accept(forClientSocket, forClientEndpoint, ec);
		if (!ec) {
			strcpy(sourceIP, forClientEndpoint.address().to_string().c_str());
			sourcePort = forClientEndpoint.port();
			forClientSocket.receive(boost::asio::buffer(socksReceiveBuffer, socksReceiveBufferSize));
			for (int i = 0; i < 16; i++)
				cout << (unsigned int)socksReceiveBuffer[i] << " ";
			cout << endl;
			if (socksReceiveBuffer[0] != 4) {
				cout << "Don't matching the socks4 protocol." << endl;
				exit(2);
			}
			mode = (unsigned int)socksReceiveBuffer[1];
			destinationPort = ((unsigned int)socksReceiveBuffer[2]) * 256 + ((unsigned int)socksReceiveBuffer[3]);
			memset(destinationIP, 0, sizeof(destinationIP));
			sprintf(destinationIP, "%u.%u.%u.%u", (unsigned int)socksReceiveBuffer[4], (unsigned int)socksReceiveBuffer[5], (unsigned int)socksReceiveBuffer[6], (unsigned int)socksReceiveBuffer[7]);
			ioservice.notify_fork(boost::asio::io_service::fork_prepare);
			pid_t pid = fork();
			if (pid == 0) {
				ioservice.notify_fork(boost::asio::io_service::fork_child);
				if (!permit_from_my_firewall()) {
					show_status(false);
					exit(1);
				}
				if (mode == 1)
					exit(socks_connect());
				else if (mode == 2)
					exit(socks_bind());
				else {
					cout << "unknown mode." << endl;
					exit(mode);
				}
			}
			else if (pid < 0) cout << "fork error!" << endl;
			else {
				ioservice.notify_fork(boost::asio::io_service::fork_parent);
				forClientSocket.close();
			}
			return 1;
		}
		else {
			cout << ec.message() << endl;
			sleep(1);
			return 0;
		}
	}
private:
	void show_status(bool isSuccess) {
		cout << "<S_IP>: " << sourceIP << endl;
		cout << "<S_PORT>: " << sourcePort << endl;
		cout << "<D_IP>: " << destinationIP << endl;
		cout << "<D_PORT>: " << destinationPort << endl;
		if (mode == 1)
			cout << "<Command>: CONNECT" << endl;
		else if (mode == 1)
			cout << "<Command>: BIND" << endl;
		if (isSuccess)
			cout << "<Reply>: Accept" << endl;
		else
			cout << "<Reply>: Reject" << endl;
	}
	bool permit_from_my_firewall() {
		string line;
		ifstream conf("socks.conf");
		while (getline(conf, line)) {
			if ((mode == 1 && line.compare(7, 2, "c ") == 0 && line.compare(0, 7, "permit ") == 0) || (mode == 2 && line.compare(7, 2, "b ") == 0 && line.compare(0, 7, "permit ") == 0)) {
				for (int i = 0, j = 9; j < line.length(); i++, j++) {
					if (destinationIP[i] != line[j] && line[j] == '*') {
						while (destinationIP[i] != '.') i++;
						j++;
					}
					else if (destinationIP[i] != line[j])
						break;
					if (i >= strlen(destinationIP))
						return true;
				}
			}
			else
				break;
		}
		return false;
	}

	int socks_bind() {
		ip::tcp::acceptor bindAcceptor(ioservice, ip::tcp::endpoint(ip::tcp::v4(), 0));
		unsigned char reply[8] = { 0 };
		reply[1] = 90;
		reply[2] = bindAcceptor.local_endpoint().port() / 256;
		reply[3] = bindAcceptor.local_endpoint().port() % 256;
		forClientSocket.send(boost::asio::buffer(reply, 8));
		bindAcceptor.accept(forRemoteSocket, forRemoteEndpoint, ec);
		if (!ec) {
			cout << "=====Accept the remote host!=====" << endl;
			forClientSocket.send(boost::asio::buffer(reply, 8));
			show_status(true);
			sleep(1);
			forClientSocket.async_receive(boost::asio::buffer(data, dataBufferSize), boost::bind(&SocksServerMainSocket::C_receive_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
			forRemoteSocket.async_receive(boost::asio::buffer(data, dataBufferSize), boost::bind(&SocksServerMainSocket::R_receive_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
			ioservice.run();
			return 0;
		}
		else {
			cout << ec.message() << endl;
			reply[2] = 91;
			forClientSocket.send(boost::asio::buffer(reply, 8));
			show_status(false);
			return 3;
		}
	}
	int socks_connect() {
		unsigned char reply[8] = { 0 };
		for (int i = 2; i < 8; i++)
			reply[i] = socksReceiveBuffer[i];
		ip::tcp::endpoint destinationEndpoint(boost::asio::ip::make_address(destinationIP), destinationPort);
		forRemoteSocket.connect(destinationEndpoint, ec);
		if (!ec) {
			cout << "=====Connect to remote host success!=====" << endl;
			reply[1] = 90;
			forClientSocket.send(boost::asio::buffer(reply, 8));
			show_status(true);
			forClientSocket.async_receive(boost::asio::buffer(data, dataBufferSize), boost::bind(&SocksServerMainSocket::C_receive_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
			forRemoteSocket.async_receive(boost::asio::buffer(data, dataBufferSize), boost::bind(&SocksServerMainSocket::R_receive_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
			ioservice.run();
			return 0;
		}
		else {
			cout << ec.message() << endl;
			reply[1] = 91;
			forClientSocket.send(boost::asio::buffer(reply, 8));
			show_status(false);
			return 3;
		}
	}
	void C_receive_handler(const boost::system::error_code& error, std::size_t bytes_transferred) {
		if (!ec) {
			cout << "Sending......" << forClientSocket.remote_endpoint().port() << ", " << forClientSocket.local_endpoint().port() << ", " << forRemoteSocket.local_endpoint().port() << ", " << forRemoteSocket.remote_endpoint().port() << ", " << bytes_transferred;
			cout << ", " << boost::asio::write(forRemoteSocket, boost::asio::buffer(data, bytes_transferred), ec) << endl;
			if (ec) {
				cout << ec.message() << endl;
			}
			if (bytes_transferred == 0) {
				ioservice.stop();
				exit(0);
			}
			forClientSocket.async_receive(boost::asio::buffer(data, dataBufferSize), boost::bind(&SocksServerMainSocket::C_receive_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		}
		else {
			cout << ec.message() << endl;
			exit(1);
		}
	}
	void R_receive_handler(const boost::system::error_code& error, std::size_t bytes_transferred) {
		if (!ec) {
			cout << "Receiving...." << forClientSocket.remote_endpoint().port() << ", " << forClientSocket.local_endpoint().port() << ", " << forRemoteSocket.local_endpoint().port() << ", " << forRemoteSocket.remote_endpoint().port() << ", " << bytes_transferred;
			cout << ", " << boost::asio::write(forClientSocket, boost::asio::buffer(data, bytes_transferred), ec) << endl;
			if (ec) {
				cout << ec.message() << endl;
			}
			if (bytes_transferred == 0) {
				ioservice.stop();
				exit(0);
			}
			forRemoteSocket.async_receive(boost::asio::buffer(data, dataBufferSize), boost::bind(&SocksServerMainSocket::R_receive_handler, this, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
		}
		else {
			cout << ec.message() << endl;
			exit(1);
		}
	}

};
int main(int argc, char *const argv[])
{
	if (argc != 2) {
		std::cerr << "Usage:" << argv[0] << " [port]" << endl;
		return 1;
	}

	signal(SIGCHLD, sig_handler);

	try {
		SocksServerMainSocket mainSocket(atoi(argv[1]));
		while (1) {
			mainSocket.accept();
		}
	}
	catch (exception &e) {
		cerr << "Exception: " << e.what() << "\n";
	}

	return 0;
}
