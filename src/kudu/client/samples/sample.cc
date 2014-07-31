// Copyright (c) 2014, Cloudera, inc.

#include <iostream>
#include <sstream>
#include <tr1/memory>

#include "kudu/client/client.h"
#include "kudu/client/encoded_key.h"
#include "kudu/client/row_result.h"
#include "kudu/common/partial_row.h"
#include "kudu/gutil/bind.h"
#include "kudu/gutil/logging.h"

using kudu::client::KuduClient;
using kudu::client::KuduClientBuilder;
using kudu::client::KuduColumnRangePredicate;
using kudu::client::KuduColumnSchema;
using kudu::client::KuduEncodedKey;
using kudu::client::KuduEncodedKeyBuilder;
using kudu::client::KuduError;
using kudu::client::KuduInsert;
using kudu::client::KuduRowResult;
using kudu::client::KuduScanner;
using kudu::client::KuduSchema;
using kudu::client::KuduSession;
using kudu::client::KuduTable;
using kudu::KuduPartialRow;
using kudu::Status;

using std::string;
using std::stringstream;
using std::vector;
using std::tr1::shared_ptr;

static Status CreateClient(const string& addr,
                           shared_ptr<KuduClient>* client) {
  return KuduClientBuilder()
      .master_server_addr(addr)
      .Build(client);
}

static KuduSchema CreateSchema() {
  const uint32_t kNonNullDefault = 12345;
  vector<KuduColumnSchema> columns;
  columns.push_back(KuduColumnSchema("key", KuduColumnSchema::UINT32));
  columns.push_back(KuduColumnSchema("int_val", KuduColumnSchema::UINT32));
  columns.push_back(KuduColumnSchema("string_val", KuduColumnSchema::STRING));
  columns.push_back(KuduColumnSchema("non_null_with_default", KuduColumnSchema::UINT32, false,
                                     &kNonNullDefault));
  return KuduSchema(columns, 1);
}

static Status DoesTableExist(const shared_ptr<KuduClient>& client,
                             const string& table_name,
                             bool *exists) {
  scoped_refptr<KuduTable> table;
  Status s = client->OpenTable(table_name, &table);
  if (s.ok()) {
    *exists = true;
  } else if (s.IsNotFound()) {
    *exists = false;
    s = Status::OK();
  }
  return s;
}

static Status CreateTable(const shared_ptr<KuduClient>& client,
                          const string& table_name,
                          const KuduSchema& schema,
                          int num_tablets) {
  // Generate the split keys for the table.
  KuduEncodedKeyBuilder key_builder(schema);
  gscoped_ptr<KuduEncodedKey> key;
  vector<string> splits;
  uint32_t increment = 1000 / num_tablets;
  for (uint32_t i = 1; i < num_tablets; i++) {
    uint32_t val = i * increment;
    key_builder.Reset();
    key_builder.AddColumnKey(&val);
    key.reset(key_builder.BuildEncodedKey());
    splits.push_back(key->ToString());
  }

  // Create the table.
  return client->NewTableCreator()
      ->table_name(table_name)
      .schema(&schema)
      .split_keys(splits)
      .Create();
}

static Status AlterTable(const shared_ptr<KuduClient>& client,
                         const string& table_name) {
  return client->NewTableAlterer()
      ->table_name(table_name)
      .rename_column("int_val", "integer_val")
      .add_nullable_column("another_val", KuduColumnSchema::BOOL)
      .drop_column("string_val")
      .Alter();
}

static void StatusCB(const Status& status) {
  LOG(INFO) << "Asynchronous flush finished with status: " << status.ToString();
}

static Status InsertRows(scoped_refptr<KuduTable>& table, int num_rows) {
  shared_ptr<KuduSession> session = table->client()->NewSession();
  RETURN_NOT_OK(session->SetFlushMode(KuduSession::MANUAL_FLUSH));
  session->SetTimeoutMillis(5000);

  for (int i = 0; i < num_rows; i++) {
    gscoped_ptr<KuduInsert> insert = table->NewInsert();
    KuduPartialRow* row = insert->mutable_row();
    RETURN_NOT_OK(row->SetUInt32("key", i));
    RETURN_NOT_OK(row->SetUInt32("integer_val", i * 2));
    RETURN_NOT_OK(row->SetUInt32("non_null_with_default", i * 5));
    RETURN_NOT_OK(session->Apply(insert.Pass()));
  }
  Status s = session->Flush();
  if (s.ok()) {
    return s;
  }

  // Test asynchronous flush.
  session->FlushAsync(Bind(&StatusCB));

  // Look at the session's errors.
  vector<KuduError*> errors;
  bool overflow;
  session->GetPendingErrors(&errors, &overflow);
  s = overflow ? Status::IOError("Overflowed pending errors in session") :
      errors.front()->status();
  while (!errors.empty()) {
    delete errors.back();
    errors.pop_back();
  }
  return s;
}

static Status ScanRows(scoped_refptr<KuduTable>& table) {
  uint32_t lower_bound = 5;
  uint32_t upper_bound = 600;
  KuduColumnRangePredicate pred(table->schema().Column(0),
                                &lower_bound, &upper_bound);

  KuduScanner scanner(table.get());
  RETURN_NOT_OK(scanner.AddConjunctPredicate(pred));
  RETURN_NOT_OK(scanner.Open());
  vector<KuduRowResult> results;
  while (scanner.HasMoreRows()) {
    RETURN_NOT_OK(scanner.NextBatch(&results));
    for (vector<KuduRowResult>::iterator iter = results.begin();
        iter != results.end();
        iter++, lower_bound++) {
      const KuduRowResult& result = *iter;
      uint32_t val;
      RETURN_NOT_OK(result.GetUInt32("key", &val));
      if (val != lower_bound) {
        stringstream out;
        out << "Scan returned the wrong results. Expected key "
            << lower_bound << " but got " << val;
        return Status::IOError(out.str());
      }
    }
    results.clear();
  }
  if (lower_bound != upper_bound) {
    stringstream out;
    out << "Scan returned the wrong results. Expected "
        << upper_bound << " rows but got " << lower_bound;
    return Status::IOError(out.str());
  }
  return Status::OK();
}

int main(int argc, char* argv[]) {
  const string kTableName = "test_table";

  // Create and connect a client.
  shared_ptr<KuduClient> client;
  CHECK_OK(CreateClient("127.0.0.1", &client));
  LOG(INFO) << "Created a client connection";

  // Create a schema.
  KuduSchema schema(CreateSchema());
  LOG(INFO) << "Created a schema";

  // Create a table with that schema.
  bool exists;
  CHECK_OK(DoesTableExist(client, kTableName, &exists));
  if (exists) {
    client->DeleteTable(kTableName);
    LOG(INFO) << "Deleting old table before creating new one";
  }
  CHECK_OK(CreateTable(client, kTableName, schema, 10));
  LOG(INFO) << "Created a table";

  // Alter the table.
  CHECK_OK(AlterTable(client, kTableName));
  LOG(INFO) << "Altered a table";

  // Insert some rows into the table.
  scoped_refptr<KuduTable> table;
  CHECK_OK(client->OpenTable(kTableName, &table));
  CHECK_OK(InsertRows(table, 1000));
  LOG(INFO) << "Inserted some rows into a table";

  // Scan some rows.
  CHECK_OK(ScanRows(table));
  LOG(INFO) << "Scanned some rows out of a table";

  // Delete the table.
  CHECK_OK(client->DeleteTable(kTableName));
  LOG(INFO) << "Deleted a table";

  // Done!
  LOG(INFO) << "Done";
  return 0;
}