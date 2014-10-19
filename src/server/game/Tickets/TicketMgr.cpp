#include "Common.h"
#include "TicketMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Chat.h"
#include "World.h"

inline float GetAge(uint64 t) { return float(time(NULL) - t) / DAY; }

///////////////////////////////////////////////////////////////////////////////////////////////////
// GM ticket
GmTicket::GmTicket() { }

GmTicket::GmTicket(Player* player, WorldPacket& recv_data) : _createTime(time(NULL)), _lastModifiedTime(time(NULL)), _closedBy(0), _assignedTo(0), _completed(false), _escalatedStatus(TICKET_UNASSIGNED)
{
    _id = sTicketMgr->GenerateTicketId();
    _playerName = player->GetName();
    _playerGuid = player->GetGUID();

    uint32 mapId;
    recv_data >> mapId;
    _mapId = mapId;

    recv_data >> _posX;
    recv_data >> _posY;
    recv_data >> _posZ;
    recv_data >> _message;

    uint32 unk1;
    recv_data >> unk1;          // not sure what this is... replyTo?
    uint8 needResponse;
    recv_data >> needResponse;  // always 1/0 -- not sure what retail does with this
}

GmTicket::~GmTicket() { }

bool GmTicket::LoadFromDB(Field* fields)
{
    uint8 index = 0;
    _id                 = fields[  index].GetUInt32();
    _playerGuid         = MAKE_NEW_GUID(fields[++index].GetUInt32(), 0, HIGHGUID_PLAYER);
    _playerName         = fields[++index].GetString();
    _message            = fields[++index].GetString();
    _createTime         = fields[++index].GetUInt32();
    _mapId              = fields[++index].GetUInt16();
    _posX               = fields[++index].GetFloat();
    _posY               = fields[++index].GetFloat();
    _posZ               = fields[++index].GetFloat();
    _lastModifiedTime   = fields[++index].GetUInt32();
    _closedBy           = fields[++index].GetInt32();
    _assignedTo         = MAKE_NEW_GUID(fields[++index].GetUInt32(), 0, HIGHGUID_PLAYER);
    _comment            = fields[++index].GetString();
    _completed          = fields[++index].GetBool();
    _escalatedStatus    = GMTicketEscalationStatus(fields[++index].GetUInt8());
    _viewed             = fields[++index].GetBool();
    return true;
}

void GmTicket::SaveToDB(SQLTransaction& trans) const
{
    uint8 index = 0;
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_REP_GM_TICKET);
    stmt->setUInt32(  index, _id);
    stmt->setUInt32(++index, GUID_LOPART(_playerGuid));
    stmt->setString(++index, _playerName);
    stmt->setString(++index, _message);
    stmt->setUInt32(++index, uint32(_createTime));
    stmt->setUInt16(++index, _mapId);
    stmt->setFloat (++index, _posX);
    stmt->setFloat (++index, _posY);
    stmt->setFloat (++index, _posZ);
    stmt->setUInt32(++index, uint32(_lastModifiedTime));
    stmt->setInt32 (++index, GUID_LOPART(_closedBy));
    stmt->setUInt32(++index, GUID_LOPART(_assignedTo));
    stmt->setString(++index, _comment);
    stmt->setBool  (++index, _completed);
    stmt->setUInt8 (++index, uint8(_escalatedStatus));
    stmt->setBool  (++index, _viewed);

    CharacterDatabase.ExecuteOrAppend(trans, stmt);
}

void GmTicket::DeleteFromDB()
{
    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_GM_TICKET);
    stmt->setUInt32(0, _id);
    CharacterDatabase.Execute(stmt);
}

void GmTicket::WritePacket(WorldPacket& data) const
{
    data << GetAge(_lastModifiedTime);
    if (GmTicket* ticket = sTicketMgr->GetOldestOpenTicket())
        data << GetAge(ticket->GetLastModifiedTime());
    else
        data << float(0);

    // I am not sure how blizzlike this is, and we don't really have a way to find out
    data << GetAge(sTicketMgr->GetLastChange());

    data << uint8(std::min(_escalatedStatus, TICKET_IN_ESCALATION_QUEUE));                              // escalated data
    data << uint8(_viewed ? GMTICKET_OPENEDBYGM_STATUS_OPENED : GMTICKET_OPENEDBYGM_STATUS_NOT_OPENED); // whether or not it has been viewed
}

void GmTicket::SendResponse(WorldSession* session) const
{
    WorldPacket data(SMSG_GMRESPONSE_RECEIVED);
    data << uint32(1); // unk? Zor says "hasActiveTicket"
    data << uint32(0); // can-edit - always 1 or 0, not flags
    data << _message.c_str();
    data << _response.c_str();
    // 3 null strings
    data << uint8(0);
    data << uint8(0);
    data << uint8(0);
    session->SendPacket(&data);
}

std::string GmTicket::FormatMessageString(ChatHandler& handler, bool detailed) const
{
    time_t curTime = time(NULL);

    std::stringstream ss;
    ss << handler.playerLink(_playerName).c_str() << ": |cffaaffaaTicket|r|cffaaccff(" << _id << ")|r ";
    ss << "|cff00ff00Created|r:|cff00ccff " << (secsToTimeString(curTime - _createTime, true, false)).c_str() << " ago|r ";
    ss << "|cff00ff00Last change|r:|cff00ccff " << (secsToTimeString(curTime - _lastModifiedTime, true, false)).c_str() << " ago|r ";

    std::string name;
    if (sObjectMgr->GetPlayerNameByGUID(_assignedTo, name))
        ss << "|cff00ff00Assigned to|r:|cff00ccff " << name.c_str() << "|r ";

    if (detailed)
    {
        ss << "|cff00ff00Ticket Message|r: [" << _message.c_str() << "]|r";
        if (!_comment.empty())
            ss << "|cff00ff00GM Comment|r: [" << _comment.c_str() << "]|r";
    }

    return ss.str();
}

std::string GmTicket::FormatMessageString(ChatHandler& handler, const char* completedByName, const char* closedByName, const char* deletedByName, const char* assignedToName, const char* unassignedByName) const
{
    std::stringstream ss;
    ss << handler.playerLink(_playerName).c_str() << ": |cffaaffaaTicket|r|cffaaccff(" << _id << ")|r ";

    if (completedByName)
        ss << "|cff00ff00Completed by|r:|cff00ccff " << completedByName << "|r ";

    if (closedByName)
        ss << "|cff00ff00Closed by|r:|cff00ccff " << closedByName << "|r ";

    if (deletedByName)
        ss << "|cff00ff00Deleted by|r:|cff00ccff " << deletedByName << "|r ";

    if (assignedToName)
        ss << "|cff00ff00Assigned to|r:|cff00ccff " << assignedToName << "|r ";

    if (unassignedByName)
        ss << "|cff00ff00Unassigned by|r:|cff00ccff " << unassignedByName << "|r ";

    return ss.str();
}

void GmTicket::SetUnassigned()
{
    _assignedTo = 0;
    switch (_escalatedStatus)
    {
        case TICKET_ASSIGNED: _escalatedStatus = TICKET_UNASSIGNED; break;
        case TICKET_ESCALATED_ASSIGNED: _escalatedStatus = TICKET_IN_ESCALATION_QUEUE; break;
        case TICKET_UNASSIGNED:
        case TICKET_IN_ESCALATION_QUEUE:
        default:
            break;
    }
}

void GmTicket::TeleportTo(Player* player) const
{
    player->TeleportTo(_mapId, _posX, _posY, _posZ, 1, 0);
}

///////////////////////////////////////////////////////////////////////////////////////////////////
// Ticket manager
TicketMgr::TicketMgr() : _status(true), _lastTicketId(0), _lastSurveyId(0), _openTicketCount(0), _lastChange(time(NULL)) {}

TicketMgr::~TicketMgr()
{
    for (GmTicketList::const_iterator itr = _ticketList.begin(); itr != _ticketList.end(); ++itr)
        delete itr->second;
}

void TicketMgr::Initialize()
{
    SetStatus(sWorld->getBoolConfig(CONFIG_ALLOW_TICKETS));
}

void TicketMgr::ResetTickets()
{
    for (GmTicketList::const_iterator itr = _ticketList.begin(); itr != _ticketList.end(); ++itr)
        if (itr->second->IsClosed())
            sTicketMgr->RemoveTicket(itr->second->GetId());

    _lastTicketId = 0;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_DEL_ALL_GM_TICKETS);

    CharacterDatabase.Execute(stmt);
}

void TicketMgr::LoadTickets()
{
    uint32 oldMSTime = getMSTime();

    for (GmTicketList::const_iterator itr = _ticketList.begin(); itr != _ticketList.end(); ++itr)
        delete itr->second;
    _ticketList.clear();

    _lastTicketId = 0;
    _openTicketCount = 0;

    PreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_GM_TICKETS);
    PreparedQueryResult result = CharacterDatabase.Query(stmt);
    if (!result)
    {
        sLog->outString(">> Loaded 0 GM tickets. DB table `gm_tickets` is empty!");
        sLog->outString();
        return;
    }

    uint32 count = 0;
    do
    {
        Field* fields = result->Fetch();
        GmTicket* ticket = new GmTicket();
        if (!ticket->LoadFromDB(fields))
        {
            delete ticket;
            continue;
        }
        if (!ticket->IsClosed())
            ++_openTicketCount;

        // Update max ticket id if necessary
        uint32 id = ticket->GetId();
        if (_lastTicketId < id)
            _lastTicketId = id;

        _ticketList[id] = ticket;
        ++count;
    } while (result->NextRow());

    sLog->outString(">> Loaded %u GM tickets in %u ms", count, GetMSTimeDiffToNow(oldMSTime));
    sLog->outString();
}

void TicketMgr::LoadSurveys()
{
    // we don't actually load anything into memory here as there's no reason to
    _lastSurveyId = 0;

    uint32 oldMSTime = getMSTime();
    if (QueryResult result = CharacterDatabase.Query("SELECT MAX(surveyId) FROM gm_surveys"))
        _lastSurveyId = (*result)[0].GetUInt32();

    sLog->outString(">> Loaded GM Survey count from database in %u ms", GetMSTimeDiffToNow(oldMSTime));
    sLog->outString();
}

void TicketMgr::AddTicket(GmTicket* ticket)
{
    _ticketList[ticket->GetId()] = ticket;
    if (!ticket->IsClosed())
        ++_openTicketCount;
    SQLTransaction trans = SQLTransaction(NULL);
    ticket->SaveToDB(trans);
}

void TicketMgr::CloseTicket(uint32 ticketId, int64 source)
{
    if (GmTicket* ticket = GetTicket(ticketId))
    {
        SQLTransaction trans = SQLTransaction(NULL);
        ticket->SetClosedBy(source);
        if (source)
            --_openTicketCount;
        ticket->SaveToDB(trans);
    }
}

void TicketMgr::RemoveTicket(uint32 ticketId)
{
    if (GmTicket* ticket = GetTicket(ticketId))
    {
        if (!ticket->IsClosed())
            --_openTicketCount;
        ticket->DeleteFromDB();
        _ticketList.erase(ticketId);
        delete ticket;
    }
}

void TicketMgr::ShowList(ChatHandler& handler, bool onlineOnly) const
{
    handler.SendSysMessage(onlineOnly ? "Showing list of open tickets whose creator is online." : "Showing list of open tickets.");
    for (GmTicketList::const_iterator itr = _ticketList.begin(); itr != _ticketList.end(); ++itr)
        if (!itr->second->IsClosed() && !itr->second->IsCompleted())
            if (!onlineOnly || itr->second->GetPlayer())
                handler.SendSysMessage(itr->second->FormatMessageString(handler).c_str());
}

void TicketMgr::ShowClosedList(ChatHandler& handler) const
{
    handler.SendSysMessage("Showing list of closed tickets.");
    for (GmTicketList::const_iterator itr = _ticketList.begin(); itr != _ticketList.end(); ++itr)
        if (itr->second->IsClosed())
            handler.SendSysMessage(itr->second->FormatMessageString(handler).c_str());
}

void TicketMgr::ShowEscalatedList(ChatHandler& handler) const
{
    handler.SendSysMessage("Showing list of escalated tickets.");
    for (GmTicketList::const_iterator itr = _ticketList.begin(); itr != _ticketList.end(); ++itr)
        if (!itr->second->IsClosed() && itr->second->GetEscalatedStatus() == TICKET_IN_ESCALATION_QUEUE)
            handler.SendSysMessage(itr->second->FormatMessageString(handler).c_str());
}

void TicketMgr::SendTicket(WorldSession* session, GmTicket* ticket) const
{
    uint32 status = GMTICKET_STATUS_DEFAULT;
    std::string message;
    if (ticket)
    {
        message = ticket->GetMessage();
        status = GMTICKET_STATUS_HASTEXT;
    }

    WorldPacket data(SMSG_GMTICKET_GETTICKET, (4 + 4 + (ticket ? message.length() + 1 + 4 + 4 + 4 + 1 + 1 : 0)));
    data << uint32(status);         // standard 0x0A, 0x06 if text present
    data << uint32(1);              // g_HasActiveGMTicket -- not a flag

    if (ticket)
    {
        data << message.c_str();    // ticket text
        data << uint8(0x7);         // ticket category; why is this hardcoded? does it make a diff re: client?

        // we've got the easy stuff done by now.
        // Now we need to go through the client logic for displaying various levels of ticket load
        if (ticket)
            ticket->WritePacket(data);
        else
        {
            // we can't actually get any numbers here...
            data << float(0);
            data << float(0);
            data << float(1);
            data << uint8(0);
            data << uint8(0);
        }
    }
    session->SendPacket(&data);
}