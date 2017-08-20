#pragma once
#include <sstream>
#include <string>
#include <vector>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>
#include <iostream>
#include <thread>
#include <boost/thread.hpp>
#include <algorithm>
#include <cstdlib>
#include <deque>
#include <list>
#include <set>
#include <mutex>
#include <condition_variable>
#include <boost/bind.hpp>
#include <boost/shared_ptr.hpp>
#include <boost/scoped_ptr.hpp>
#include <boost/enable_shared_from_this.hpp>
#include "boost/date_time/posix_time/posix_time_types.hpp"
#include <queue>
#include <boost/asio.hpp>
class tcp_connection
	: public boost::enable_shared_from_this<tcp_connection>
{
public:
	typedef boost::shared_ptr<tcp_connection> pointer;

	static pointer create(boost::asio::io_service& io_service)
	{
		return pointer(new tcp_connection(io_service));
	}

	boost::asio::ip::tcp::socket& socket()
	{
		return socket_;
	}

	void onComplete(boost::shared_ptr<std::string> s, const boost::system::error_code& error_code, size_t byteswritten)
	{
		
		if (byteswritten == (size_t)0 && s->empty())
		{
			s.reset();
			this->interrupt();
		}
		else
		{
			s.reset();
		}
		
	}
	
	void sendMsg(std::string& msg)
	{
		std::unique_lock<std::mutex> msgMu(this->messageMutex);
		this->msgs.push(msg);
		msgMu.unlock();
		this->messageVar.notify_one();
	}

	void writeMsgs()
	{
		while (this->isRunning == true)
		{
			std::unique_lock<std::mutex> msgMu(this->messageMutex);
			this->messageVar.wait(msgMu);
			if (this->socket().is_open() != false && this->isRunning == true)
			{
				if (this->msgs.size() > 0)
				{
					boost::shared_ptr<std::string> s = boost::make_shared<std::string>(this->msgs.front());
					boost::asio::async_write(this->socket(), boost::asio::buffer(*s), boost::bind(&tcp_connection::onComplete, this, s, boost::asio::placeholders::error, boost::asio::placeholders::bytes_transferred));
					this->msgs.pop();
				}
				msgMu.unlock();
			}
			else
			{
				msgMu.unlock();
				break;
			}
		}

	}

	void interrupt()
	{
		std::unique_lock<std::mutex> msgMu(this->messageMutex);
		while (this->msgs.size() > 0)
			msgs.pop();
		if(this->socket().is_open())
			this->socket().close();
		msgMu.unlock();
		this->isRunning = false;
		
		this->messageVar.notify_one();

		
	}

	void accepted()
	{
		this->isRunning = true;
		this->sendingThread = std::thread(&tcp_connection::writeMsgs, this);
	}

	bool IsRunning() { return this->isRunning; }
	~tcp_connection()
	{
		if(this->sendingThread.joinable())
			this->sendingThread.join();
	}
private:
	tcp_connection(boost::asio::io_service& io_service)
		: socket_(io_service)
	{

	}

	boost::asio::ip::tcp::socket socket_;
	std::queue<std::string> msgs;

	std::mutex messageMutex;
	std::condition_variable messageVar;

	std::thread sendingThread;

	volatile bool isRunning;

};

class tcp_server
{
public:
	tcp_server(boost::asio::io_service& io_service)
		: acceptor_(io_service, boost::asio::ip::tcp::endpoint(boost::asio::ip::tcp::v4(), 30001))
	{
		this->shutdownBeginning = false;
		this->shutdownCompleted = false;
		std::unique_lock<std::mutex> conMutex(this->connectionMutex);
		this->connections = std::map<int, boost::shared_ptr<tcp_connection>>();
		this->connectionsToRemove = std::vector<int>();
		conMutex.unlock();
		
		this->sendingThread = std::thread(&tcp_server::processMsgs,this);

		start_accept();

	}
	void BroadcastMessage(std::string &msg)
	{
		if (!this->shutdownBeginning)
		{
			std::unique_lock<std::mutex> msgMu(this->messageMutex);
			this->msgs.push(msg);
			msgMu.unlock();
			this->messageVar.notify_one();
		}
	}

	void interrupt()
	{
		this->shutdownBeginning = true;
		this->messageVar.notify_one();
	}

	~tcp_server()
	{
		this->sendingThread.join();
	}
	bool IsShutDownCompleted()
	{
		return this->shutdownCompleted;
	}
	
private:
	void start_accept()
	{
		tcp_connection::pointer new_connection =
			tcp_connection::create(acceptor_.get_io_service());

		acceptor_.async_accept(new_connection->socket(),
			boost::bind(&tcp_server::handle_accept, this, new_connection,
				boost::asio::placeholders::error));
	}


#pragma region Handle the Connection Acceptance
	void handle_accept(tcp_connection::pointer new_connection,
		const boost::system::error_code& error)
	{
		
		if (!error && !this->shutdownBeginning)
		{
			new_connection->accepted();
			addConnection(std::move(new_connection));
			start_accept();
		}
	}
#pragma endregion

#pragma region Add Connection
	void addConnection(boost::shared_ptr<tcp_connection> tcp)
	{
		if (!this->shutdownBeginning)
		{
			std::unique_lock<std::mutex> conMutex(this->connectionMutex);
			this->connections[this->connections.size()] = std::move(tcp);
			conMutex.unlock();
		}
	}
#pragma endregion

	void processMsgs()
	{
		while (this->shutdownBeginning == false)
		{
			std::unique_lock<std::mutex> msgMu(this->messageMutex);
			
			this->messageVar.wait(msgMu);
			if (this->msgs.size() > 0 && !this->shutdownBeginning)
			{

				std::unique_lock<std::mutex> conMu(this->connectionMutex);
				
				for (std::pair<int, boost::shared_ptr<tcp_connection>> con : this->connections)
				{
					if (con.second->IsRunning() && con.second->socket().is_open())
					{
						con.second->sendMsg(this->msgs.front());
					}
					else
					{
						this->connectionsToRemove.push_back(con.first);
					
					}
				}
				if (this->connectionsToRemove.size() > 0)
				{
					for (int ri : this->connectionsToRemove)
					{
						this->connections.erase(ri);
					}
					this->connectionsToRemove.clear();
				}
				conMu.unlock();
				this->msgs.pop();
			}
			else if (this->shutdownBeginning != false)
			{
				std::unique_lock<std::mutex> conMu(this->connectionMutex);
				for (std::pair<int, boost::shared_ptr<tcp_connection>> con : this->connections)
				{
					con.second->interrupt();
				}
				this->connections.clear();
				while (this->msgs.size() > 0)
				{
					this->msgs.pop();
				}
				this->connectionsToRemove.clear();
				conMu.unlock();
				msgMu.unlock();
				this->shutdownCompleted = true;
				return;
			}
			
			msgMu.unlock();
			
		}
	}

	

	boost::asio::ip::tcp::acceptor acceptor_;
	std::thread sendingThread;
	std::queue<std::string> msgs;


	std::mutex messageMutex;

	std::mutex connectionMutex;



	std::condition_variable messageVar;

	std::map<int,boost::shared_ptr<tcp_connection>> connections;

	std::vector<int> connectionsToRemove;
	
	volatile bool shutdownBeginning;
	volatile bool shutdownCompleted;
};

