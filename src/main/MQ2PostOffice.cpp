/*
 * MacroQuest: The extension platform for EverQuest
 * Copyright (C) 2002-2023 MacroQuest Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2, as published by
 * the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "pch.h"
#include "MQ2PostOffice.h"
#include "CrashHandler.h"

#include "MQ2Main.h"

#include <mq/proto/PostOffice.h>

namespace mq {
using namespace postoffice;

// MQModule forward declarations
namespace pipeclient {
static void PulsePostOffice();
static void SetGameStatePostOffice(DWORD);
}

static MQModule s_PostOfficeModule = {
	"PostOffice",
	false,
	nullptr,                                   // Initialize
	nullptr,                                   // Shutdown
	pipeclient::PulsePostOffice,               // Pulse
	pipeclient::SetGameStatePostOffice,        // SetGameState
	nullptr,                                   // UpdateImGui
	nullptr,                                   // Zoned
	nullptr,                                   // WriteChatColor
	nullptr,                                   // SpawnAdded
	nullptr,                                   // SpawnRemoved
	nullptr,                                   // BeginZone
	nullptr,                                   // EndZone
	nullptr,                                   // LoadPlugin
	nullptr                                    // UnloadPlugin
};
MQModule* GetPostOfficeModule() { return &s_PostOfficeModule; }

class MQ2PostOffice : public PostOffice
{
private:

	class PipeEventsHandler : public NamedPipeEvents
	{
	public:
		PipeEventsHandler(MQ2PostOffice* postOffice) : m_postOffice(postOffice) {}

		virtual void OnIncomingMessage(PipeMessagePtr&& message) override
		{
			switch (message->GetMessageId())
			{
			case MQMessageId::MSG_ROUTE:
			{
				auto envelope = ProtoMessage::Parse<proto::Envelope>(message);
				const auto& address = envelope.address();
				// either this message is coming off the pipe, so assume it was routed correctly by the server,
				// or it was routed internally after checking to make sure that the destination of the message
				// was within the client. In either case, we can safely assume that we should route it to an
				// internal mailbox
				if (address.has_mailbox())
				{
					m_postOffice->DeliverTo(address.mailbox(), std::move(message));
				}
				else
				{
					// This is a failsafe action, we shouldn't expect to be here often. For this code to
					// be reached, we would have to have a client that packages a message in an envelope
					// that is intended to be parsed directly by the server and not routed anywhere (so
					// no mailbox routing information is included), rather than just send the message
					m_postOffice->DeliverTo("pipe_client", std::move(message));
				}

				break;
			}

			case MQMessageId::MSG_MAIN_CRASHPAD_CONFIG:
				// Message needs to at least have some substance...
				if (message->size() > 0)
				{
					std::string pipeName{ message->get<const char>() };

					if (pipeName.empty())
					{
						InitializeCrashpad();
					}
					else
					{
						InitializeCrashpadPipe(pipeName);
					}
				}
				break;

			case MQMessageId::MSG_MAIN_REQ_UNLOAD:
				HideDoCommand(pLocalPlayer, "/unload", true);
				break;

			case MQMessageId::MSG_MAIN_REQ_FORCEUNLOAD:
				HideDoCommand(pLocalPlayer, "/unload force", true);
				break;

			case MQMessageId::MSG_MAIN_PROCESS_LOADED: {
				// This is the response to our connection message below.
				if (message->size() >= sizeof(MQMessageProcessLoadedResponse))
				{
					const MQMessageProcessLoadedResponse* response = message->get<MQMessageProcessLoadedResponse>();
					m_postOffice->m_launcherProcessID = response->processId;

					SPDLOG_DEBUG("Launcher process ID: {}", m_postOffice->m_launcherProcessID);
				}
				break;
			}

			case MQMessageId::MSG_MAIN_FOCUS_ACTIVATE_WND:
				if (message->size() >= sizeof(MQMessageActivateWnd))
				{
					const MQMessageActivateWnd* request = message->get<MQMessageActivateWnd>();

					pipeclient::RequestActivateWindow((HWND)request->hWnd, false);
				}
				break;

			default: break;
			}
		}

		virtual void OnClientConnected() override
		{
			//SPDLOG_DEBUG("Connection to named pipe created, Sending process loaded message.");

			MQMessageProcessLoadedFromMQ msg;
			msg.processId = GetCurrentProcessId();
			m_postOffice->m_pipeClient.SendMessage(MQMessageId::MSG_MAIN_PROCESS_LOADED, &msg, sizeof(msg));

			pipeclient::SetGameStatePostOffice(0);
		}

	private:
		MQ2PostOffice* m_postOffice;
	};

public:

	MQ2PostOffice() : m_pipeClient{ mq::MQ2_PIPE_SERVER_PATH }, m_launcherProcessID(0)
	{
		m_pipeClient.SetHandler(std::make_shared<PipeEventsHandler>(this));
		m_pipeClient.Start();
		::atexit(StopPipeClient);

		m_clientMailbox = CreateAndAddMailbox("pipe_client",
			[this](ProtoMessagePtr&& message)
			{
				m_pipeClient.DispatchMessage(std::move(message));
			});
	}

	~MQ2PostOffice()
	{
		RemoveMailbox("pipe_client");
		m_clientMailbox.reset();
		m_pipeClient.Stop();
	}

	void RouteMessage(PipeMessagePtr&& message) override
	{
		if (message->GetMessageId() == MQMessageId::MSG_ROUTE)
		{
			auto envelope = ProtoMessage::Parse<proto::Envelope>(message);
			if (envelope.has_address())
			{
				auto address = envelope.address();
				if ((address.has_pid() && address.pid() != GetCurrentProcessId()) || address.has_name() || address.has_account() || address.has_server() || address.has_character())
				{
					// we can't assume that even if we match the address (account/server/character) that this
					// client is the only one that does. We need to route it through the server to ensure that
					// it gets to all clients that match
					m_pipeClient.SendMessage(std::move(message));
				}
				else if (address.has_mailbox())
				{
					DeliverTo(address.mailbox(), std::move(message));
				}
				else
				{
					DeliverTo("pipe_client", std::move(message));
				}
			}
			else
			{
				DeliverTo("pipe_client", std::move(message));
			}
		}
		else
		{
			// not a route, just send it to the server
			m_pipeClient.SendMessage(std::move(message));
		}
	}

	void RouteMessage(const void* data, size_t length) override
	{
		RouteMessage(std::make_unique<PipeMessage>(MQMessageId::MSG_ROUTE, data, length));
	}

	void ProcessPipeClient()
	{
		m_pipeClient.Process();
		Process(10);
	}

	void NotifyIsForegroundWindow(bool isForeground)
	{
		MQMessageFocusRequest request;
		request.focusMode = MQMessageFocusRequest::FocusMode::HasFocus;
		request.state = isForeground;
		request.processId = GetCurrentProcessId();

		m_pipeClient.SendMessage(MQMessageId::MSG_MAIN_FOCUS_REQUEST, &request, sizeof(request));
	}

	void RequestActivateWindow(HWND hWnd, bool sendMessage)
	{
		if (IsIconic(hWnd))
		{
			ShowWindow(hWnd, SW_RESTORE);
		}

		if (::SetForegroundWindow(hWnd))
			return;

		if (sendMessage && m_pipeClient.IsConnected())
		{
			MQMessageFocusRequest request;
			request.focusMode = MQMessageFocusRequest::FocusMode::WantFocus;
			request.hWnd = hWnd;

			m_pipeClient.SendMessage(MQMessageId::MSG_MAIN_FOCUS_REQUEST, &request, sizeof(request));
			return;
		}

		ShowWindow(hWnd, SW_MINIMIZE);
		ShowWindow(hWnd, SW_RESTORE);
	}

	void SetGameStatePostOffice(DWORD GameState)
	{
		proto::Identification id;
		id.set_pid(GetCurrentProcessId()); // we should always have a pid

		const char* login = GetLoginName();
		if (login != nullptr)
			id.set_account(login);

		if (pEverQuestInfo != nullptr)
		{
			const char* server = GetServerShortName();
			if (server != nullptr)
				id.set_server(server);
		}

		if (pLocalPC != nullptr && pLocalPC->Name != nullptr)
		{
			id.set_character(pLocalPC->Name);
		}

		m_pipeClient.SendProtoMessage(MQMessageId::MSG_IDENTIFICATION, id);
	}

private:
	ProtoPipeClient m_pipeClient;
	std::shared_ptr<PostOffice::Mailbox> m_clientMailbox;
	DWORD m_launcherProcessID;

	static void StopPipeClient()
	{
		static_cast<MQ2PostOffice&>(GetPostOffice()).m_pipeClient.Stop();
	}
};

PostOffice& postoffice::GetPostOffice() {
	static MQ2PostOffice s_postOffice;
	return s_postOffice;
}

namespace pipeclient {

void NotifyIsForegroundWindow(bool isForeground)
{
	static_cast<MQ2PostOffice&>(GetPostOffice()).NotifyIsForegroundWindow(isForeground);
}

void RequestActivateWindow(HWND hWnd, bool sendMessage)
{
	static_cast<MQ2PostOffice&>(GetPostOffice()).RequestActivateWindow(hWnd, sendMessage);
}

void PulsePostOffice()
{
	static_cast<MQ2PostOffice&>(GetPostOffice()).ProcessPipeClient();
}

void SetGameStatePostOffice(DWORD GameState)
{
	static_cast<MQ2PostOffice&>(GetPostOffice()).SetGameStatePostOffice(GameState);
}

} // namespace pipeclient

} // namespace mq