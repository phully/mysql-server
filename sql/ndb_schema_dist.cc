/*
   Copyright (c) 2011, 2019, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License, version 2.0,
   as published by the Free Software Foundation.

   This program is also distributed with certain software (including
   but not limited to OpenSSL) that is licensed under separate terms,
   as designated in a particular file or component or in included license
   documentation.  The authors of MySQL hereby grant you an additional
   permission to link the program and your derivative works with the
   separately licensed software that they have included with MySQL.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License, version 2.0, for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

// Implements the functions declared in ndb_schema_dist.h
#include "sql/ndb_schema_dist.h"

#include <atomic>

#include "my_dbug.h"
#include "ndbapi/ndb_cluster_connection.hpp"
#include "sql/ndb_anyvalue.h"
#include "sql/ndb_name_util.h"
#include "sql/ndb_require.h"
#include "sql/ndb_schema_dist_table.h"
#include "sql/ndb_schema_result_table.h"
#include "sql/ndb_share.h"
#include "sql/ndb_thd.h"
#include "sql/ndb_thd_ndb.h"
#include "sql/query_options.h"          // OPTION_BIN_LOG
#include "sql/sql_thd_internal_api.h"

// Temporarily use a fixed string on the form "./mysql/ndb_schema" as key
// for retrieving the NDB_SHARE for mysql.ndb_schema. This will subsequently
// be removed when a NDB_SHARE can be acquired using db+table_name and the
// key is formatted behind the curtains in NDB_SHARE without using
// build_table_filename() etc.
static constexpr const char* NDB_SCHEMA_TABLE_KEY =
    IF_WIN(".\\mysql\\ndb_schema", "./mysql/ndb_schema");

bool Ndb_schema_dist::is_ready(void* requestor) {
  DBUG_TRACE;

  std::stringstream ss;
  ss << "is_ready_" << std::hex << requestor;
  const std::string reference = ss.str();

  NDB_SHARE* schema_share =
      NDB_SHARE::acquire_reference_by_key(NDB_SCHEMA_TABLE_KEY,
                                          reference.c_str());
  if (schema_share == nullptr)
    return false; // Not ready

  if (!schema_share->have_event_operation()) {
    NDB_SHARE::release_reference(schema_share, reference.c_str());
    return false; // Not ready
  }

  NDB_SHARE::release_reference(schema_share, reference.c_str());
  return true;
}


bool Ndb_schema_dist_client::is_schema_dist_table(const char* db,
                                                  const char* table_name)
{
  if (db == Ndb_schema_dist_table::DB_NAME &&
      table_name == Ndb_schema_dist_table::TABLE_NAME)
  {
    // This is the NDB table used for schema distribution
    return true;
  }
  return false;
}

bool Ndb_schema_dist_client::is_schema_dist_result_table(
    const char* db, const char* table_name) {
  if (db == Ndb_schema_result_table::DB_NAME &&
      table_name == Ndb_schema_result_table::TABLE_NAME) {
    // This is the NDB table used for schema distribution results
    return true;
  }
  return false;
}

Ndb_schema_dist_client::Ndb_schema_dist_client(THD* thd)
    : m_thd(thd), m_thd_ndb(get_thd_ndb(thd)) {}

bool Ndb_schema_dist_client::prepare(const char* db, const char* tabname)
{
  DBUG_ENTER("Ndb_schema_dist_client::prepare");

  // Acquire reference on mysql.ndb_schema
  // NOTE! Using fixed "reference", assuming only one Ndb_schema_dist_client
  // is started at a time since it requires GSL. This may have to be revisited
  m_share =
      NDB_SHARE::acquire_reference_by_key(NDB_SCHEMA_TABLE_KEY,
                                          "ndb_schema_dist_client");
  if (m_share == nullptr ||
      m_share->have_event_operation() == false ||
      DBUG_EVALUATE_IF("ndb_schema_dist_not_ready_early", true, false))
  {
    // The NDB_SHARE for mysql.ndb_schema hasn't been created or not setup
    // yet -> schema distribution is not ready
    m_thd_ndb->push_warning("Schema distribution is not ready");
    DBUG_RETURN(false);
  }

  // Save the prepared "keys"(which are used when communicating with
  // the other MySQL Servers), they should match the keys used in later calls.
  m_prepared_keys.add_key(db, tabname);

  Ndb_schema_dist_table schema_dist_table(m_thd_ndb);
  if (!schema_dist_table.open()) {
    DBUG_RETURN(false);
  }

  if (!schema_dist_table.check_schema()) {
    DBUG_RETURN(false);
  }

  // Open the ndb_schema_result table, the table is created by ndbcluster
  // when connecting to NDB and thus it shall exist at this time.
  Ndb_schema_result_table schema_result_table(m_thd_ndb);
  if (!schema_result_table.open()) {
    DBUG_RETURN(false);
  }

  if (!schema_result_table.check_schema()) {
    DBUG_RETURN(false);
  }

  // Schema distribution is ready
  DBUG_RETURN(true);
}

bool Ndb_schema_dist_client::prepare_rename(const char* db, const char* tabname,
                                            const char* new_db,
                                            const char* new_tabname) {
  DBUG_ENTER("Ndb_schema_dist_client::prepare_rename");

  // Normal prepare first
  if (!prepare(db, tabname))
  {
    DBUG_RETURN(false);
  }

  // Allow additional keys for rename which will use the "old" name
  // when communicating with participants until the rename is done.
  // After rename has occurred, the new name will be used
  m_prepared_keys.add_key(new_db, new_tabname);

  // Schema distribution is ready
  DBUG_RETURN(true);
}

bool Ndb_schema_dist_client::check_identifier_limits(
    std::string& invalid_identifier) {
  DBUG_ENTER("Ndb_schema_dist_client::check_identifier_limits");

  Ndb_schema_dist_table schema_dist_table(m_thd_ndb);
  if (!schema_dist_table.open()) {
    invalid_identifier = "<open failed>";
    DBUG_RETURN(false);
  }

  // Check that identifiers does not exceed the limits imposed
  // by the ndb_schema table layout
  for (auto key: m_prepared_keys.keys())
  {
    // db
    if (!schema_dist_table.check_column_identifier_limit(
            Ndb_schema_dist_table::COL_DB, key.first)) {
      invalid_identifier = key.first;
      DBUG_RETURN(false);
    }
    // name
    if (!schema_dist_table.check_column_identifier_limit(
            Ndb_schema_dist_table::COL_NAME, key.second)) {
      invalid_identifier = key.second;
      DBUG_RETURN(false);
    }
  }
  DBUG_RETURN(true);
}

void Ndb_schema_dist_client::Prepared_keys::add_key(const char* db,
                                                    const char* tabname) {
  m_keys.emplace_back(db, tabname);
}

bool Ndb_schema_dist_client::Prepared_keys::check_key(
    const char* db, const char* tabname) const {
  for (auto key : m_keys) {
    if (key.first == db && key.second == tabname) {
      return true;  // OK, key has been prepared
    }
  }
  return false;
}

extern void update_slave_api_stats(const Ndb*);

Ndb_schema_dist_client::~Ndb_schema_dist_client()
{
  if (m_share)
  {
    // Release the reference to mysql.ndb_schema table
    NDB_SHARE::release_reference(m_share, "ndb_schema_dist_client");
  }

  if (m_thd_ndb->is_slave_thread())
  {
    // Copy-out slave thread statistics
    // NOTE! This is just a "convenient place" to call this
    // function, it could be moved to "end of statement"(if there
    // was such a place..).
    update_slave_api_stats(m_thd_ndb->ndb);
  }
}

/*
  Produce unique identifier for distributing objects that
  does not have any global id from NDB. Use a sequence counter
  which is unique in this node.
*/
static std::atomic<uint32> schema_dist_id_sequence{0};
uint32 Ndb_schema_dist_client::unique_id() const {
  uint32 id = ++schema_dist_id_sequence;
  // Handle wraparound
  if (id == 0) {
    id = ++schema_dist_id_sequence;
  }
  DBUG_ASSERT(id != 0);
  return id;
}

/*
  Produce unique identifier for distributing objects that
  does not have any global version from NDB. Use own nodeid
  which is unique in NDB.
*/
uint32 Ndb_schema_dist_client::unique_version() const {
  const uint32 ver = m_thd_ndb->connection->node_id();
  DBUG_ASSERT(ver != 0);
  return ver;
}

bool Ndb_schema_dist_client::log_schema_op(const char* query,
                                           size_t query_length, const char* db,
                                           const char* table_name, uint32 id,
                                           uint32 version, SCHEMA_OP_TYPE type,
                                           bool log_query_on_participant) {
  DBUG_ENTER("Ndb_schema_dist_client::log_schema_op");
  DBUG_ASSERT(db && table_name);
  DBUG_ASSERT(id != 0 && version != 0);
  DBUG_ASSERT(m_thd_ndb);

  // Never allow temporary names when communicating with participant
  if (ndb_name_is_temp(db) || ndb_name_is_temp(table_name))
  {
    DBUG_ASSERT(false);
    DBUG_RETURN(false);
  }

  // Require that m_share has been initialized to reference the
  // schema distribution table
  ndbcluster::ndbrequire(m_share);

  // Check that prepared keys match
  if (!m_prepared_keys.check_key(db, table_name))
  {
    m_thd_ndb->push_warning("INTERNAL ERROR: prepared keys didn't match");
    DBUG_ASSERT(false);  // Catch in debug
    DBUG_RETURN(false);
  }

  // Don't distribute if thread has turned off schema distribution
  if (m_thd_ndb->check_option(Thd_ndb::NO_LOG_SCHEMA_OP)) {
    DBUG_PRINT("info", ("NO_LOG_SCHEMA_OP set - > skip schema distribution"));
    DBUG_RETURN(true); // Ok, skipped
  }

  // Verify identifier limits, this should already have been caught earlier
  {
    std::string invalid_identifier;
    if (!check_identifier_limits(invalid_identifier))
    {
      m_thd_ndb->push_warning("INTERNAL ERROR: identifier limits exceeded");
      DBUG_ASSERT(false); // Catch in debug
      DBUG_RETURN(false);
    }
  }

  // Calculate anyvalue
  const Uint32 anyvalue = calculate_anyvalue(log_query_on_participant);

  const int result = log_schema_op_impl(
      m_thd_ndb->ndb, query, static_cast<int>(query_length), db, table_name, id,
      version, type, anyvalue);
  if (result != 0) {
    // Schema distribution failed
    m_thd_ndb->push_warning("Schema distribution failed!");
    DBUG_RETURN(false);
  }
  DBUG_RETURN(true);
}

bool Ndb_schema_dist_client::create_table(const char* db,
                                          const char* table_name, int id,
                                          int version) {
  DBUG_ENTER("Ndb_schema_dist_client::create_table");

  if (is_schema_dist_table(db, table_name))
  {
    // Create of the schema distribution table is not distributed. Instead,
    // every MySQL Server have special handling to create it if not
    // exists and then open it as first step of connecting to the cluster
    DBUG_RETURN(true);
  }

  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            db, table_name, id, version, SOT_CREATE_TABLE));
}

bool Ndb_schema_dist_client::truncate_table(const char* db,
                                            const char* table_name, int id,
                                            int version) {
  DBUG_ENTER("Ndb_schema_dist_client::truncate_table");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            db, table_name, id, version, SOT_TRUNCATE_TABLE));
}



bool Ndb_schema_dist_client::alter_table(const char* db, const char* table_name,
                                         int id, int version,
                                         bool log_on_participant) {
  DBUG_ENTER("Ndb_schema_dist_client::alter_table");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            db, table_name, id, version, SOT_ALTER_TABLE_COMMIT,
                            log_on_participant));
}

bool Ndb_schema_dist_client::alter_table_inplace_prepare(const char* db,
                                                         const char* table_name,
                                                         int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::alter_table_inplace_prepare");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            db, table_name, id, version,
                            SOT_ONLINE_ALTER_TABLE_PREPARE));
}

bool Ndb_schema_dist_client::alter_table_inplace_commit(const char* db,
                                                        const char* table_name,
                                                        int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::alter_table_inplace_commit");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            db, table_name, id, version,
                            SOT_ONLINE_ALTER_TABLE_COMMIT));
}

bool Ndb_schema_dist_client::rename_table_prepare(
    const char* db, const char* table_name, int id, int version,
    const char* new_key_for_table) {
  DBUG_ENTER("Ndb_schema_dist_client::rename_table_prepare");
  // NOTE! The rename table prepare phase is primarily done in order to
  // pass the "new key"(i.e db/table_name) for the table to be renamed,
  // that's since there isn't enough placeholders in the subsequent rename
  // table phase.
  DBUG_RETURN(log_schema_op(new_key_for_table, strlen(new_key_for_table), db,
                            table_name, id, version, SOT_RENAME_TABLE_PREPARE));
}

bool Ndb_schema_dist_client::rename_table(const char* db,
                                          const char* table_name, int id,
                                          int version, const char* new_dbname,
                                          const char* new_tabname,
                                          bool log_on_participant) {
  DBUG_ENTER("Ndb_schema_dist_client::rename_table");

  /*
    Rewrite the query, the original query may contain several tables but
    rename_table() is called once for each table in the query.
      ie. RENAME TABLE t1 to tx, t2 to ty;
          -> RENAME TABLE t1 to tx + RENAME TABLE t2 to ty
  */
  std::string rewritten_query;
  rewritten_query.append("rename table `")
      .append(db)
      .append("`.`")
      .append(table_name)
      .append("` to `")
      .append(new_dbname)
      .append("`.`")
      .append(new_tabname)
      .append("`");
  DBUG_PRINT("info", ("rewritten query: '%s'", rewritten_query.c_str()));

  DBUG_RETURN(log_schema_op(rewritten_query.c_str(), rewritten_query.length(),
                            db, table_name, id, version, SOT_RENAME_TABLE,
                            log_on_participant));
}

bool Ndb_schema_dist_client::drop_table(const char *db, const char *table_name,
                                        int id, int version,
                                        bool log_on_participant) {
  DBUG_ENTER("Ndb_schema_dist_client::drop_table");

  /*
    Never distribute each dropped table as part of DROP DATABASE:
    1) as only the DROP DATABASE command should go into binlog
    2) as this MySQL Server is dropping the tables from NDB, when
       the participants get the DROP DATABASE it will remove
       any tables from the DD and then remove the database.
  */
  DBUG_ASSERT(thd_sql_command(m_thd) != SQLCOM_DROP_DB);

  /*
    Rewrite the query, the original query may contain several tables but
    drop_table() is called once for each table in the query.
    ie. DROP TABLE t1, t2;
      -> DROP TABLE t1 + DROP TABLE t2
  */
  std::string rewritten_query;
  rewritten_query.append("drop table `")
      .append(db)
      .append("`.`")
      .append(table_name)
      .append("`");
  DBUG_PRINT("info", ("rewritten query: '%s'", rewritten_query.c_str()));

  // Special case where the table to be dropped was already dropped in the
  // client. This is considered acceptable behavior and the query is distributed
  // to ensure that the table is dropped in the pariticipants. Assign values to
  // id and version to workaround the assumption that they will always be != 0
  if (id == 0 && version == 0) {
    id = unique_id();
    version = unique_version();
  }

  DBUG_RETURN(log_schema_op(rewritten_query.c_str(), rewritten_query.length(),
                            db, table_name, id, version, SOT_DROP_TABLE,
                            log_on_participant));
}

bool Ndb_schema_dist_client::create_db(const char* query, uint query_length,
                                       const char* db, unsigned int id,
                                       unsigned int version) {
  DBUG_ENTER("Ndb_schema_dist_client::create_db");

  // Checking identifier limits "late", there is no way to return
  // an error to fail the CREATE DATABASE command
  std::string invalid_identifier;
  if (!check_identifier_limits(invalid_identifier))
  {
    // Check of db name limit failed
    m_thd_ndb->push_warning("Identifier name '%-.100s' is too long",
                            invalid_identifier.c_str());
    DBUG_RETURN(false);
  }

  DBUG_RETURN(log_schema_op(query, query_length, db, "",
                            id, version, SOT_CREATE_DB));
}

bool Ndb_schema_dist_client::alter_db(const char* query, uint query_length,
                                      const char* db, unsigned int id,
                                      unsigned int version) {
  DBUG_ENTER("Ndb_schema_dist_client::alter_db");

  // Checking identifier limits "late", there is no way to return
  // an error to fail the ALTER DATABASE command
  std::string invalid_identifier;
  if (!check_identifier_limits(invalid_identifier))
  {
    // Check of db name limit failed
    m_thd_ndb->push_warning("Identifier name '%-.100s' is too long",
                            invalid_identifier.c_str());
    DBUG_RETURN(false);
  }

  DBUG_RETURN(log_schema_op(query, query_length, db, "",
                            id, version, SOT_ALTER_DB));
}

bool Ndb_schema_dist_client::drop_db(const char* db) {
  DBUG_ENTER("Ndb_schema_dist_client::drop_db");

  // Checking identifier limits "late", there is no way to return
  // an error to fail the DROP DATABASE command
  std::string invalid_identifier;
  if (!check_identifier_limits(invalid_identifier))
  {
    // Check of db name limit failed
    m_thd_ndb->push_warning("Identifier name '%-.100s' is too long",
                            invalid_identifier.c_str());
    DBUG_RETURN(false);
  }

  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            db, "", unique_id(), unique_version(),
                            SOT_DROP_DB));
}

bool Ndb_schema_dist_client::acl_notify(const char* query, uint query_length,
                                        const char* db) {
  DBUG_ENTER("Ndb_schema_dist_client::acl_notify");
  DBUG_RETURN(log_schema_op(query, query_length, db, "", unique_id(),
                            unique_version(), SOT_GRANT));
}

bool Ndb_schema_dist_client::tablespace_changed(const char* tablespace_name,
                                                int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::tablespace_changed");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", tablespace_name, id, version, SOT_TABLESPACE));
}

bool Ndb_schema_dist_client::logfilegroup_changed(const char* logfilegroup_name,
                                                  int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::logfilegroup_changed");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", logfilegroup_name, id, version,
                            SOT_LOGFILE_GROUP));
}

bool Ndb_schema_dist_client::create_tablespace(const char* tablespace_name,
                                               int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::create_tablespace");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", tablespace_name, id, version,
                            SOT_CREATE_TABLESPACE));
}

bool Ndb_schema_dist_client::alter_tablespace(const char* tablespace_name,
                                              int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::alter_tablespace");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", tablespace_name, id, version,
                            SOT_ALTER_TABLESPACE));
}

bool Ndb_schema_dist_client::drop_tablespace(const char* tablespace_name,
                                             int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::drop_tablespace");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", tablespace_name, id, version,
                            SOT_DROP_TABLESPACE));
}

bool
Ndb_schema_dist_client::create_logfile_group(const char* logfile_group_name,
                                             int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::create_logfile_group");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", logfile_group_name, id, version,
                            SOT_CREATE_LOGFILE_GROUP));
}

bool
Ndb_schema_dist_client::alter_logfile_group(const char* logfile_group_name,
                                            int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::alter_logfile_group");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", logfile_group_name, id, version,
                            SOT_ALTER_LOGFILE_GROUP));
}

bool
Ndb_schema_dist_client::drop_logfile_group(const char* logfile_group_name,
                                           int id, int version) {
  DBUG_ENTER("Ndb_schema_dist_client::drop_logfile_group");
  DBUG_RETURN(log_schema_op(ndb_thd_query(m_thd), ndb_thd_query_length(m_thd),
                            "", logfile_group_name, id, version,
                            SOT_DROP_LOGFILE_GROUP));
}

const char*
Ndb_schema_dist_client::type_name(SCHEMA_OP_TYPE type)
{
  switch(type){
  case SOT_DROP_TABLE:
    return "DROP_TABLE";
  case SOT_CREATE_TABLE:
    return "CREATE_TABLE";
  case SOT_ALTER_TABLE_COMMIT:
    return "ALTER_TABLE_COMMIT";
  case SOT_DROP_DB:
    return "DROP_DB";
  case SOT_CREATE_DB:
    return "CREATE_DB";
  case SOT_ALTER_DB:
    return "ALTER_DB";
  case SOT_CLEAR_SLOCK:
    return "CLEAR_SLOCK";
  case SOT_TABLESPACE:
    return "TABLESPACE";
  case SOT_LOGFILE_GROUP:
    return "LOGFILE_GROUP";
  case SOT_RENAME_TABLE:
    return "RENAME_TABLE";
  case SOT_TRUNCATE_TABLE:
    return "TRUNCATE_TABLE";
  case SOT_RENAME_TABLE_PREPARE:
    return "RENAME_TABLE_PREPARE";
  case SOT_ONLINE_ALTER_TABLE_PREPARE:
    return "ONLINE_ALTER_TABLE_PREPARE";
  case SOT_ONLINE_ALTER_TABLE_COMMIT:
    return "ONLINE_ALTER_TABLE_COMMIT";
  case SOT_CREATE_USER:
    return "CREATE_USER";
  case SOT_DROP_USER:
    return "DROP_USER";
  case SOT_RENAME_USER:
    return "RENAME_USER";
  case SOT_GRANT:
    return "GRANT";
  case SOT_REVOKE:
    return "REVOKE";
  case SOT_CREATE_TABLESPACE:
    return "CREATE_TABLESPACE";
  case SOT_ALTER_TABLESPACE:
    return "ALTER_TABLESPACE";
  case SOT_DROP_TABLESPACE:
    return "DROP_TABLESPACE";
  case SOT_CREATE_LOGFILE_GROUP:
    return "CREATE_LOGFILE_GROUP";
  case SOT_ALTER_LOGFILE_GROUP:
    return "ALTER_LOGFILE_GROUP";
  case SOT_DROP_LOGFILE_GROUP:
    return "DROP_LOGFILE_GROUP";
  default:
    break;
  }
  DBUG_ASSERT(false);
  return "<unknown>";
}

uint32 Ndb_schema_dist_client::calculate_anyvalue(bool force_nologging) const {
  Uint32 anyValue = 0;
  if (!thd_slave_thread(m_thd)) {
    /* Schema change originating from this MySQLD, check SQL_LOG_BIN
     * variable and pass 'setting' to all logging MySQLDs via AnyValue
     */
    if (thd_test_options(m_thd, OPTION_BIN_LOG)) /* e.g. SQL_LOG_BIN == on */
    {
      DBUG_PRINT("info", ("Schema event for binlogging"));
      ndbcluster_anyvalue_set_normal(anyValue);
    } else {
      DBUG_PRINT("info", ("Schema event not for binlogging"));
      ndbcluster_anyvalue_set_nologging(anyValue);
    }

    if (!force_nologging) {
      DBUG_PRINT("info", ("Forcing query not to be binlogged on participant"));
      ndbcluster_anyvalue_set_nologging(anyValue);
    }
  } else {
    /*
       Slave propagating replicated schema event in ndb_schema
       In case replicated serverId is composite
       (server-id-bits < 31) we copy it into the
       AnyValue as-is
       This is for 'future', as currently Schema operations
       do not have composite AnyValues.
       In future it may be useful to support *not* mapping composite
       AnyValues to/from Binlogged server-ids.
    */
    DBUG_PRINT("info", ("Replicated schema event with original server id"));
    anyValue = thd_unmasked_server_id(m_thd);
  }

#ifndef DBUG_OFF
  /*
    MySQLD will set the user-portion of AnyValue (if any) to all 1s
    This tests code filtering ServerIds on the value of server-id-bits.
  */
  const char *p = getenv("NDB_TEST_ANYVALUE_USERDATA");
  if (p != 0 && *p != 0 && *p != '0' && *p != 'n' && *p != 'N') {
    dbug_ndbcluster_anyvalue_set_userbits(anyValue);
  }
#endif
  DBUG_PRINT("info", ("anyvalue: %u", anyValue));
  return anyValue;
}
