/*
* Copyright (C) 2009-2012 Rajko Stojadinovic <http://github.com/rajkosto/hive>
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program; if not, write to the Free Software
* Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include "SqlCharDataSource.h"
#include "Database/Database.h"

#include <boost/lexical_cast.hpp>
using boost::lexical_cast;
using boost::bad_lexical_cast;

SqlCharDataSource::SqlCharDataSource( Poco::Logger& logger, shared_ptr<Database> db, const string& idFieldName, const string& wsFieldName ) : SqlDataSource(logger,db)
{
	_idFieldName = getDB()->escape_string(idFieldName);
	_wsFieldName = getDB()->escape_string(wsFieldName);
}

SqlCharDataSource::~SqlCharDataSource() {}

Sqf::Value SqlCharDataSource::fetchCharacterInitial( string playerId, int serverId, const string& playerName )
{
	bool newPlayer = false;
	//make sure player exists in db
	{
		scoped_ptr<QueryResult> playerRes(getDB()->PQuery(("SELECT `PlayerName`, `PlayerSex` FROM `Player_DATA` WHERE `"+_idFieldName+"`='%s'").c_str(), getDB()->escape_string(playerId).c_str()));
		if (playerRes)
		{
			newPlayer = false;
			Field* fields = playerRes->Fetch();
			//update player name if not current
			if (fields[0].GetCppString() != playerName)
			{
				static SqlStatementID stmtId;
				scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, "UPDATE `Player_DATA` SET `PlayerName`=? WHERE `"+_idFieldName+"`=?"));
				stmt->addString(playerName);
				stmt->addString(playerId);
				bool exRes = stmt->Execute();
				poco_assert(exRes == true);
				_logger.information("Changed name of player " + playerId + " from '" + fields[0].GetCppString() + "' to '" + playerName + "'");
			}
		}
		else
		{
			newPlayer = true;
			//insert new player into db
			static SqlStatementID stmtId;
			scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, "INSERT INTO `Player_DATA` (`"+_idFieldName+"`, `PlayerName`) VALUES (?, ?)"));
			stmt->addString(playerId);
			stmt->addString(playerName);
			bool exRes = stmt->Execute();
			poco_assert(exRes == true);
			_logger.information("Created a new player " + playerId + " named '" + playerName + "'");
		}
	}

	//get characters from db
	scoped_ptr<QueryResult> charsRes(getDB()->PQuery(
		("SELECT `CharacterID`, `"+_wsFieldName+"`, `Inventory`, `Backpack`, "
		"TIMESTAMPDIFF(MINUTE,`Datestamp`,`LastLogin`) as `SurvivalTime`, "
		"TIMESTAMPDIFF(MINUTE,`LastAte`,NOW()) as `MinsLastAte`, "
		"TIMESTAMPDIFF(MINUTE,`LastDrank`,NOW()) as `MinsLastDrank`, "
		"`Model` FROM `Character_DATA` WHERE `"+_idFieldName+"` = '%s' AND `Alive` = 1 ORDER BY `CharacterID` DESC LIMIT 1").c_str(), getDB()->escape_string(playerId).c_str()));

	bool newChar = false; //not a new char
	int characterId = -1; //invalid charid
	Sqf::Value worldSpace = Sqf::Parameters(); //empty worldspace
	Sqf::Value inventory = lexical_cast<Sqf::Value>("[]"); //empty inventory
	Sqf::Value backpack = lexical_cast<Sqf::Value>("[]"); //empty backpack
	Sqf::Value survival = lexical_cast<Sqf::Value>("[0,0,0]"); //0 mins alive, 0 mins since last ate, 0 mins since last drank
	string model = ""; //empty models will be defaulted by scripts
	if (charsRes)
	{
		Field* fields = charsRes->Fetch();
		newChar = false;
		characterId = fields[0].GetInt32();
		try
		{
			worldSpace = lexical_cast<Sqf::Value>(fields[1].GetCppString());
		}
		catch(bad_lexical_cast)
		{
			_logger.warning("Invalid Worldspace for CharacterID("+lexical_cast<string>(characterId)+"): "+fields[1].GetCppString());
		}
		if (!fields[2].IsNULL()) //inventory can be null
		{
			try
			{
				inventory = lexical_cast<Sqf::Value>(fields[2].GetCppString());
				try { SanitiseInv(boost::get<Sqf::Parameters>(inventory)); } catch (const boost::bad_get&) {}
			}
			catch(bad_lexical_cast)
			{
				_logger.warning("Invalid Inventory for CharacterID("+lexical_cast<string>(characterId)+"): "+fields[2].GetCppString());
			}
		}		
		if (!fields[3].IsNULL()) //backpack can be null
		{
			try
			{
				backpack = lexical_cast<Sqf::Value>(fields[3].GetCppString());
			}
			catch(bad_lexical_cast)
			{
				_logger.warning("Invalid Backpack for CharacterID("+lexical_cast<string>(characterId)+"): "+fields[3].GetCppString());
			}
		}
		//set survival info
		{
			Sqf::Parameters& survivalArr = boost::get<Sqf::Parameters>(survival);
			survivalArr[0] = fields[4].GetInt32();
			survivalArr[1] = fields[5].GetInt32();
			survivalArr[2] = fields[6].GetInt32();
		}
		try
		{
			model = boost::get<string>(lexical_cast<Sqf::Value>(fields[7].GetCppString()));
		}
		catch(...)
		{
			model = fields[7].GetCppString();
		}

		//update last login
		{
			//update last character login
			static SqlStatementID stmtId;
			scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, "UPDATE `Character_DATA` SET `LastLogin` = CURRENT_TIMESTAMP WHERE `CharacterID` = ?"));
			stmt->addInt32(characterId);
			bool exRes = stmt->Execute();
			poco_assert(exRes == true);
		}
	}
	else //inserting new character
	{
		newChar = true;

		int generation = 1;
		int humanity = 2500;
		//try getting previous character info
		{
			scoped_ptr<QueryResult> prevCharRes(getDB()->PQuery(
				("SELECT `Generation`, `Humanity`, `Model`, `InstanceID` FROM `Character_DATA` WHERE `"+_idFieldName+"` = '%s' AND `Alive` = 0 ORDER BY `CharacterID` DESC LIMIT 1").c_str(), getDB()->escape_string(playerId).c_str()));
			if (prevCharRes)
			{
				Field* fields = prevCharRes->Fetch();
				generation = fields[0].GetInt32();
#ifdef INCREASE_GENERATION
				generation++; //MY METAL BOY
#endif
				humanity = fields[1].GetInt32();
				try
				{
					model = boost::get<string>(lexical_cast<Sqf::Value>(fields[2].GetCppString()));
				}
				catch(...)
				{
					model = fields[2].GetCppString();
				}



			}
		}
		Sqf::Value medical = Sqf::Parameters(); //script will fill this in if empty
		//insert new char into db
		{
			//update last character login
			static SqlStatementID stmtId;
			scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, 
				"INSERT INTO `Character_DATA` (`"+_idFieldName+"`, `InstanceID`, `"+_wsFieldName+"`, `Inventory`, `Backpack`, `Medical`, `Generation`, `Datestamp`, `LastLogin`, `LastAte`, `LastDrank`, `Humanity`) "
				"VALUES (?, ?, ?, ?, ?, ?, ?, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, CURRENT_TIMESTAMP, ?)"));
			stmt->addString(playerId);
			stmt->addInt32(serverId);
			stmt->addString(lexical_cast<string>(worldSpace));
			stmt->addString(lexical_cast<string>(inventory));
			stmt->addString(lexical_cast<string>(backpack));
			stmt->addString(lexical_cast<string>(medical));
			stmt->addInt32(generation);
			stmt->addInt32(humanity);
			bool exRes = stmt->DirectExecute(); //need sync as we will be getting the CharacterID right after this
			if (exRes == false)
			{
				_logger.error("Error creating character for playerId " + playerId);
				Sqf::Parameters retVal;
				retVal.push_back(string("ERROR"));
				return retVal;
			}
		}
		//get the new character's id
		{
			scoped_ptr<QueryResult> newCharRes(getDB()->PQuery(
				("SELECT `CharacterID` FROM `Character_DATA` WHERE `"+_idFieldName+"` = '%s' AND `Alive` = 1 ORDER BY `CharacterID` DESC LIMIT 1").c_str(), getDB()->escape_string(playerId).c_str()));
			if (!newCharRes)
			{
				_logger.error("Error fetching created character for playerId " + playerId);
				Sqf::Parameters retVal;
				retVal.push_back(string("ERROR"));
				return retVal;
			}
			Field* fields = newCharRes->Fetch();
			characterId = fields[0].GetInt32();
		}
		_logger.information("Created a new character " + lexical_cast<string>(characterId) + " for player '" + playerName + "' (" + playerId + ")" );
	}

	Sqf::Parameters retVal;
	retVal.push_back(string("PASS"));
	retVal.push_back(newPlayer);
	retVal.push_back(lexical_cast<string>(characterId));
	if (!newChar)
	{
		retVal.push_back(worldSpace);
		retVal.push_back(inventory);
		retVal.push_back(backpack);
		retVal.push_back(survival);
	}
	retVal.push_back(model);
	//hive interface version
	retVal.push_back(0.96f);

	return retVal;
}

Sqf::Value SqlCharDataSource::fetchCharacterDetails( int characterId )
{
	Sqf::Parameters retVal;
	//get details from db
	scoped_ptr<QueryResult> charDetRes(getDB()->PQuery(
		"SELECT `%s`, `Medical`, `Generation`, `KillsZ`, `HeadshotsZ`, `KillsH`, `KillsB`, `CurrentState`, `Humanity`, `InstanceID` "
		"FROM `Character_DATA` WHERE `CharacterID`=%d", _wsFieldName.c_str(), characterId));

	if (charDetRes)
	{
		Sqf::Value worldSpace = Sqf::Parameters(); //empty worldspace
		Sqf::Value medical = Sqf::Parameters(); //script will fill this in if empty
		int generation = 1;
		Sqf::Value stats = lexical_cast<Sqf::Value>("[0,0,0,0]"); //killsZ, headZ, killsH, killsB
		Sqf::Value currentState = Sqf::Parameters(); //empty state (aiming, etc)
		int humanity = 2500;
		int instance = 1;
		//get stuff from row
		{
			Field* fields = charDetRes->Fetch();
			try
			{
				worldSpace = lexical_cast<Sqf::Value>(fields[0].GetCppString());
			}
			catch(bad_lexical_cast)
			{
				_logger.warning("Invalid Worldspace (detail load) for CharacterID("+lexical_cast<string>(characterId)+"): "+fields[0].GetCppString());
			}
			try
			{
				medical = lexical_cast<Sqf::Value>(fields[1].GetCppString());
			}
			catch(bad_lexical_cast)
			{
				_logger.warning("Invalid Medical (detail load) for CharacterID("+lexical_cast<string>(characterId)+"): "+fields[1].GetCppString());
			}
			generation = fields[2].GetInt32();
			//set stats
			{
				Sqf::Parameters& statsArr = boost::get<Sqf::Parameters>(stats);
				statsArr[0] = fields[3].GetInt32();
				statsArr[1] = fields[4].GetInt32();
				statsArr[2] = fields[5].GetInt32();
				statsArr[3] = fields[6].GetInt32();
			}
			try
			{
				currentState = lexical_cast<Sqf::Value>(fields[7].GetCppString());
			}
			catch(bad_lexical_cast)
			{
				_logger.warning("Invalid CurrentState (detail load) for CharacterID("+lexical_cast<string>(characterId)+"): "+fields[7].GetCppString());
			}
			humanity = fields[8].GetInt32();
			instance = fields[9].GetInt32();
		}

		retVal.push_back(string("PASS"));
		retVal.push_back(medical);
		retVal.push_back(stats);
		retVal.push_back(currentState);
		retVal.push_back(worldSpace);
		retVal.push_back(humanity);
		retVal.push_back(instance);
	}
	else
	{
		retVal.push_back(string("ERROR"));
	}

	return retVal;
}

bool SqlCharDataSource::updateCharacter( int characterId, int serverId, const FieldsType& fields )
{
	map<string,string> sqlFields;

	for (auto it=fields.begin();it!=fields.end();++it)
	{
		const string& name = it->first;
		const Sqf::Value& val = it->second;

		//arrays
		if (name == "Worldspace" || name == "Inventory" || name == "Backpack" || name == "Medical" || name == "CurrentState")
			sqlFields[name] = "'"+getDB()->escape_string(lexical_cast<string>(val))+"'";
		//booleans
		else if (name == "JustAte" || name == "JustDrank")
		{
			if (boost::get<bool>(val))
			{
				string newName = "LastAte";
				if (name == "JustDrank")
					newName = "LastDrank";

				sqlFields[newName] = "CURRENT_TIMESTAMP";
			}
		}
		//addition integeroids
		else if (name == "KillsZ" || name == "HeadshotsZ" || name == "DistanceFoot" || name == "Duration" ||
			name == "KillsH" || name == "KillsB" || name == "Humanity")
		{
			int integeroid = static_cast<int>(Sqf::GetDouble(val));
			char intSign = '+';
			if (integeroid < 0)
			{
				intSign = '-';
				integeroid = abs(integeroid);
			}

			if (integeroid > 0) 
				sqlFields[name] = "(`"+name+"` "+intSign+" "+lexical_cast<string>(integeroid)+")";
		}
		//strings
		else if (name == "Model")
			sqlFields[name] = "'"+getDB()->escape_string(boost::get<string>(val))+"'";
	}

	if (sqlFields.size() > 0)
	{
		string query = "UPDATE `Character_DATA` SET ";
		for (auto it=sqlFields.begin();it!=sqlFields.end();)
		{
			string fieldName = it->first;
			if (fieldName == "Worldspace")
				fieldName = _wsFieldName;

			query += "`" + fieldName + "` = " + it->second;
			++it;
			if (it != sqlFields.end())
				query += " , ";
		}
		query += ", `InstanceID` = " + lexical_cast<string>(serverId) + "  WHERE `CharacterID` = " + lexical_cast<string>(characterId);
		bool exRes = getDB()->Execute(query.c_str());
		poco_assert(exRes == true);

		return exRes;
	}

	return true;
}

bool SqlCharDataSource::initCharacter( int characterId, const Sqf::Value& inventory, const Sqf::Value& backpack )
{
	static SqlStatementID stmtId;
	scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, "UPDATE `Character_DATA` SET `Inventory` = ? , `Backpack` = ? WHERE `CharacterID` = ?"));
	stmt->addString(lexical_cast<string>(inventory));
	stmt->addString(lexical_cast<string>(backpack));
	stmt->addInt32(characterId);
	bool exRes = stmt->Execute();
	poco_assert(exRes == true);

	return exRes;
}

bool SqlCharDataSource::killCharacter( int characterId, int duration )
{
	static SqlStatementID stmtId;
	scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, 
		"UPDATE `Character_DATA` SET `Alive` = 0, `LastLogin` = DATE_SUB(CURRENT_TIMESTAMP, INTERVAL ? MINUTE) WHERE `CharacterID` = ? AND `Alive` = 1"));
	stmt->addInt32(duration);
	stmt->addInt32(characterId);
	bool exRes = stmt->Execute();
	poco_assert(exRes == true);

	return exRes;
}

bool SqlCharDataSource::recordLogin( string playerId, int characterId, int action )
{
	static SqlStatementID stmtId;
	scoped_ptr<SqlStatement> stmt(getDB()->CreateStatement(stmtId, 
		"INSERT INTO `Player_LOGIN` (`"+_idFieldName+"`, `CharacterID`, `Datestamp`, `Action`) VALUES (?, ?, CURRENT_TIMESTAMP, ?)"));
	stmt->addString(playerId);
	stmt->addInt32(characterId);
	stmt->addInt32(action);
	bool exRes = stmt->Execute();
	poco_assert(exRes == true);

	return exRes;
}

Sqf::Value SqlCharDataSource::fetchObjectId( Int64 objectIdent )
{
	Sqf::Parameters retVal;
	//get details from db
	scoped_ptr<QueryResult> charDetRes(getDB()->PQuery(
		"SELECT `ObjectID` FROM `Object_DATA` WHERE `ObjectUID`=%lld", objectIdent));

	if (charDetRes)
	{
		int objectid = 0;
		//get stuff from row
		{
			Field* fields = charDetRes->Fetch();
			objectid = fields[0].GetInt32();
		}
		if(objectid != 0)
		{
			retVal.push_back(string("PASS"));
			retVal.push_back(lexical_cast<string>(objectid));
		}
		else 
		{
			retVal.push_back(string("ERROR"));
		}
	}
	else
	{
		retVal.push_back(string("ERROR"));
	}

	return retVal;
}
