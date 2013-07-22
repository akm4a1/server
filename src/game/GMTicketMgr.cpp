/**
 * This code is part of MaNGOS. Contributor & Copyright details are in AUTHORS/THANKS.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"
#include "Database/DatabaseEnv.h"
#include "SQLStorages.h"
#include "GMTicketMgr.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "ProgressBar.h"
#include "Policies/Singleton.h"
#include "Player.h"

INSTANTIATE_SINGLETON_1(GMTicketMgr);

void GMTicket::SaveSurveyData(WorldPacket& recvData) const
{
    uint32 x;
    recvData >> x;                                         // answer range? (6 = 0-5?)
    DEBUG_LOG("SURVEY: X = %u", x);
    
    uint8 result[10];
    memset(result, 0, sizeof(result));
    for (int i = 0; i < 10; ++i)
    {
        uint32 questionID;
        recvData >> questionID;                            // GMSurveyQuestions.dbc
        if (!questionID)
            break;
        
        uint8 value;
        std::string unk_text;
        recvData >> value;                                 // answer
        recvData >> unk_text;                              // always empty?
        
        result[i] = value;
        DEBUG_LOG("SURVEY: ID %u, value %u, text %s", questionID, value, unk_text.c_str());
    }
    
    std::string comment;
    recvData >> comment;                                   // addional comment
    DEBUG_LOG("SURVEY: comment %s", comment.c_str());
    
    // TODO: chart this data in some way in DB
}

void GMTicket::Init(ObjectGuid guid, const std::string& text, const std::string& responsetext, time_t update)
{
    m_guid = guid;
    m_text = text;
    m_responseText = responsetext;
    m_lastUpdate = update;
}

void GMTicket::SetText(const char* text)
{
    m_text = text ? text : "";
    m_lastUpdate = time(NULL);

    std::string escapedString = m_text;
    CharacterDatabase.escape_string(escapedString);
    CharacterDatabase.PExecute("UPDATE character_ticket SET ticket_text = '%s' "
                               "WHERE guid = '%u'",
                               escapedString.c_str(), m_guid.GetCounter());
}

void GMTicket::SetResponseText(const char* text)
{
    m_responseText = text ? text : "";
    m_lastUpdate = time(NULL);

    std::string escapedString = m_responseText;
    CharacterDatabase.escape_string(escapedString);
    CharacterDatabase.PExecute("UPDATE character_ticket SET response_text = '%s' "
                               "WHERE guid = '%u'",
                               escapedString.c_str(), m_guid.GetCounter());
}

void GMTicket::DeleteFromDB() const
{
    CharacterDatabase.PExecute("DELETE FROM character_ticket "
                               "WHERE guid = '%u' LIMIT 1",
                               m_guid.GetCounter());
}

void GMTicket::SaveToDB() const
{
    CharacterDatabase.BeginTransaction();
    DeleteFromDB();

    std::string questionEscaped = m_text;
    CharacterDatabase.escape_string(questionEscaped);

    std::string responseEscaped = m_responseText;
    CharacterDatabase.escape_string(responseEscaped);

    CharacterDatabase.PExecute("INSERT INTO character_ticket (guid, ticket_text, response_text) "
                               "VALUES ('%u', '%s', '%s')",
                               m_guid.GetCounter(), questionEscaped.c_str(), responseEscaped.c_str());
    CharacterDatabase.CommitTransaction();
}

void GMTicket::CloseWithSurvey() const
{
    _Close(GM_TICKET_STATUS_SURVEY);
}

void GMTicket::Close() const
{
    _Close(GM_TICKET_STATUS_CLOSE);
}

void GMTicket::_Close(GMTicketStatus statusCode) const
{
    Player* pPlayer = sObjectMgr.GetPlayer(m_guid);
    if (!pPlayer)
        return;

    //Perhaps this should be marked as an outdated ticket instead?
    //Mark ticket as closed instead! Also, log conversation between
    //GM and player up until this point
    DeleteFromDB();
    pPlayer->GetSession()->SendGMTicketStatusUpdate(statusCode);
}

void GMTicketMgr::LoadGMTickets()
{
    m_GMTicketMap.clear();                                  // For reload case

    QueryResult* result = CharacterDatabase.Query(
                              //      0     1            2              3                                  4
                              "SELECT guid, ticket_text, response_text, UNIX_TIMESTAMP(ticket_lastchange), ticket_id FROM character_ticket ORDER BY ticket_id ASC");

    if (!result)
    {
        BarGoLink bar(1);

        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded `character_ticket`, table is empty.");
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();

        Field* fields = result->Fetch();

        uint32 guidlow = fields[0].GetUInt32();
        if (!guidlow)
            continue;

        ObjectGuid guid = ObjectGuid(HIGHGUID_PLAYER, guidlow);

        GMTicket& ticket = m_GMTicketMap[guid];

        if (ticket.GetPlayerGuid())                         // already exist
        {
            CharacterDatabase.PExecute("DELETE FROM character_ticket "
                                       "WHERE ticket_id = '%u'",
                                       fields[4].GetUInt32());
            continue;
        }

        ticket.Init(guid, fields[1].GetCppString(), fields[2].GetCppString(), time_t(fields[3].GetUInt64()));
        m_GMTicketListByCreatingOrder.push_back(&ticket);
    }
    while (result->NextRow());
    delete result;

    sLog.outString();
    sLog.outString(">> Loaded " SIZEFMTD " GM tickets", GetTicketCount());
}

void GMTicketMgr::DeleteAll()
{
    for (GMTicketMap::const_iterator itr = m_GMTicketMap.begin(); itr != m_GMTicketMap.end(); ++itr)
    {
        if (Player* owner = sObjectMgr.GetPlayer(itr->first))
            owner->GetSession()->SendGMTicketGetTicket(0x0A);
    }
    CharacterDatabase.Execute("DELETE FROM character_ticket");
    m_GMTicketListByCreatingOrder.clear();
    m_GMTicketMap.clear();
}
