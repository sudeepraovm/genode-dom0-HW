#include <dom0-HW/dom0_server.h>

#include <cstring>
#include <vector>
#include <string>

#include <base/printf.h>
#include <lwip/genode.h>
#include <os/attached_ram_dataspace.h>
#include <nic/packet_allocator.h>

#include <dom0-HW/communication_magic_numbers.h>
#include <timer_session/connection.h>
#include <os/config.h>

/* Fiasco includes */
namespace Fiasco {
#include <l4/sys/kdebug.h>
}

namespace Dom0_server {

Dom0_server::Dom0_server() :
	_listen_socket(0),
	_in_addr{0},
	_target_addr{0},
	_task_loader{},
	_parser{}
{
	lwip_tcpip_init();

	enum { BUF_SIZE = Nic::Packet_allocator::DEFAULT_PACKET_SIZE * 128 };

	Genode::Xml_node network = Genode::config()->xml_node().sub_node("network");

	_in_addr.sin_family = AF_INET;

	/* set listen port from config */
	char port[5] = {0};
	network.attribute("port").value(port, sizeof(port));

	_in_addr.sin_port = htons(atoi(port));

	/* set listen address to any */
	_in_addr.sin_addr.s_addr = INADDR_ANY;
	
	if (network.attribute_value<bool>("dhcp", true))
	{
		
		PDBG("DHCP network...");
		if (lwip_nic_init(0,
		                  0,
		                  0,
		                  BUF_SIZE,
		                  BUF_SIZE)) {
			PERR("lwip init failed!");
			return;
		}
		/* dhcp assignement takes some time... */
		PDBG("Waiting 10s for ip assignement");
		Timer::Connection timer;
		timer.msleep(10000);
	}
	else
	{
		PDBG("manual network...");
		char ip_addr[16] = {0};
		char subnet[16] = {0};
		char gateway[16] = {0};

		network.attribute("ip-address").value(ip_addr, sizeof(ip_addr));
		network.attribute("subnet-mask").value(subnet, sizeof(subnet));
		network.attribute("default-gateway").value(gateway, sizeof(gateway));

		if (lwip_nic_init(inet_addr(ip_addr),
		                  inet_addr(subnet),
		                  inet_addr(gateway),
		                  BUF_SIZE,
		                  BUF_SIZE)) {
			PERR("lwip init failed!");
			return;
		}
	}

	if ((_listen_socket = lwip_socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		PERR("No socket available!");
		return;
	}
	if (lwip_bind(_listen_socket, (struct sockaddr*)&_in_addr, sizeof(_in_addr)))
	{
		PERR("Bind failed!");
		return;
	}
	if (lwip_listen(_listen_socket, 5))
	{
		PERR("Listen failed!");
		return;
	}
	PINF("Listening...\n");
}

Dom0_server::~Dom0_server()
{
	disconnect();
}

int Dom0_server::connect()
{
	socklen_t len = sizeof(_target_addr);
	_target_socket = lwip_accept(_listen_socket, &_target_addr, &len);
	if (_target_socket < 0)
	{
		PWRN("Invalid socket from accept!");
		return _target_socket;
	}
	sockaddr_in* target_in_addr = (sockaddr_in*)&_target_addr;
	PINF("Got connection from %s", inet_ntoa(target_in_addr));
	return _target_socket;
}

// Receive data from the socket and write it into data.
ssize_t Dom0_server::Child_starter_thread::receive_data(void* data, size_t size, int _target_socket)
{
	ssize_t result = 0;
	ssize_t position = 0;
	// Because read() might actually read less than size bytes
	// before it returns, we call it in a loop
	// until size bytes have been read.
	do
	{
		result = lwip_read(_target_socket, (char*) data + position, size - position);
		if (result < 1)
		{
			return -errno;
		}
		position += result;

	} while ((size_t) position < size);

	return position;
}

// convenience function
ssize_t Dom0_server::Child_starter_thread::receiveInt32_t(int32_t& data, int _target_socket)
{
	return receive_data(&data, sizeof(data), _target_socket);
}

// Send data from buffer data with size size to the socket.
ssize_t Dom0_server::Child_starter_thread::send_data(void* data, size_t size, int _target_socket)
{
	ssize_t result = 0;
	ssize_t position = 0;

	// Because write() might actually write less than size bytes
	// before it returns, we call it in a loop
	// until size bytes have been written.
	do
	{
		result = lwip_write(_target_socket, (char*) data + position, size - position);
		if (result < 1)
			return -errno;
		position += result;

	} while ((size_t) position < size);

	return position;
}

// convenience function
ssize_t Dom0_server::Child_starter_thread::sendInt32_t(int32_t data, int _target_socket)
{
	return send_data(&data, sizeof(data), _target_socket);
}

void Dom0_server::serve()
{
	int message = 0;
	while (true)
	{
		NETCHECK_LOOP(_starter_thread.receiveInt32_t(message, _target_socket));
		if (message == SEND_DESCS)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_send_descs(_target_socket);
			PDBG("Done SEND_DESCS. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if (message == CLEAR)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_clear(_target_socket);
			PDBG("Done START. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if (message == SEND_BINARIES)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_send_binaries(_target_socket);
			PDBG("Done SEND_BINARIES. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if (message == GET_LIVE)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_send_live(_target_socket);
			PDBG("Done GET_LIVE. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if (message == START)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_start(_target_socket);
			PDBG("Done START. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if (message == STOP)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_stop(_target_socket);
			PDBG("Done STOP. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if (message == GET_PROFILE)
		{
			int time_before=timer.elapsed_ms();
			_starter_thread.do_send_profile(_target_socket);
			PDBG("Done GET_PROFILE. Took: %d",timer.elapsed_ms()-time_before);
		}
		else if(message == REBOOT)
		{
			using namespace Fiasco;
                	enter_kdebug("*#^");
		}
		else
		{
			PWRN("Unknown message: %d", message);
		}
	}
}

void Dom0_server::disconnect()
{
	lwip_close(_target_socket);
	PERR("Target socket closed.");
	lwip_close(_listen_socket);
	PERR("Server socket closed.");
}

Dom0_server::Child_starter_thread::Child_starter_thread() :
	Thread_deprecated{"child_starter"}
{
	start();
}

void Dom0_server::Child_starter_thread::do_start(int target_socket)
{
	PDBG("Starting tasks.");
	_task_loader.start();
}

void Dom0_server::Child_starter_thread::do_stop(int target_socket)
{
	PDBG("Stopping tasks.");
	_task_loader.stop();
}

void Dom0_server::Child_starter_thread::do_clear(int target_socket)
{
	PDBG("Clearing tasks.");
	_task_loader.clear_tasks();
}

void Dom0_server::Child_starter_thread::do_send_descs(int target_socket)
{
	PDBG("Ready to receive task description.");

	// Get XML size.
	int xml_size;
	receiveInt32_t(xml_size, target_socket);
	Genode::Attached_ram_dataspace xml_ds(Genode::env()->ram_session(), xml_size);
	PINF("Ready to receive XML of size %d.", xml_size);

	// Get XML file.
	receive_data(xml_ds.local_addr<char>(), xml_size,target_socket);
	PDBG("Received XML. Initializing tasks.");
	_task_loader.add_tasks(xml_ds.cap());
}

void Dom0_server::Child_starter_thread::do_send_binaries(int target_socket)
{
	PDBG("Ready to receive binaries.");

	// Get number of binaries to receive.
	int num_binaries = 0;
	receiveInt32_t(num_binaries, target_socket);
	PINF("%d binar%s to be sent.", num_binaries, num_binaries == 1 ? "y" : "ies");

	// Receive binaries.
	for (int i = 0; i < num_binaries; i++)
	{
		// Client is waiting for ready signal.
		sendInt32_t(GO_SEND, target_socket);
		// Get binary name.
		Genode::Attached_ram_dataspace name_ds(Genode::env()->ram_session(), 16);
		receive_data(name_ds.local_addr<char>(), 16, target_socket);
		// Get binary size.
		int32_t binary_size = 0;
		receiveInt32_t(binary_size, target_socket);
		// Get binary data.
		Genode::Dataspace_capability binDsCap = _task_loader.binary_ds(name_ds.cap(), binary_size);
		Genode::Region_map* rm = Genode::env()->rm_session();
		char* bin = (char*)rm->attach(binDsCap);
		receive_data(bin, binary_size, target_socket);
		PINF("Got binary '%s' of size %d.", name_ds.local_addr<char>(), binary_size);
		rm->detach(bin);
	}
}

void Dom0_server::Child_starter_thread::do_send_profile(int target_socket)
{
	Genode::Dataspace_capability xmlDsCap = _task_loader.profile_data();
	Genode::Region_map* rm = Genode::env()->rm_session();
	char* xml = (char*)rm->attach(xmlDsCap);
	size_t size = std::strlen(xml) + 1;
	PINF("Sending profile data of size %d", size);
	sendInt32_t(size, target_socket);
	send_data(xml, size, target_socket);
	rm->detach(xml);
}

void Dom0_server::Child_starter_thread::do_send_live(int target_socket)
{
	Genode::Dataspace_capability xmlDsCap = _parser.live_data();
	Genode::Region_map* rm = Genode::env()->rm_session();
	char* xml = (char*)rm->attach(xmlDsCap);
	size_t size = std::strlen(xml) + 1;
	PINF("Sending live data of size %d", size);
	sendInt32_t(size, target_socket);
	send_data(xml, size, target_socket);
	rm->detach(xml);
}

void Dom0_server::Child_starter_thread::entry()
{
	while (true)
	{
	}
}



void Dom0_server::send_profile(Genode::String<32> task_name)
{
	_controller.optimize(task_name);
	_starter_thread.do_send_profile(_target_socket);	
}

Dom0_server::Child_starter_thread Dom0_server::_starter_thread;
}
