////////////////////////////////////////////////////////////////////////////////
/// DISCLAIMER
///
/// Copyright 2014-2016 ArangoDB GmbH, Cologne, Germany
/// Copyright 2004-2014 triAGENS GmbH, Cologne, Germany
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///
/// Copyright holder is ArangoDB GmbH, Cologne, Germany
///
/// @author Dr. Frank Celler
////////////////////////////////////////////////////////////////////////////////

#include "v8-vocindex.h"
#include "Basics/conversions.h"
#include "FulltextIndex/fulltext-index.h"
#include "Indexes/CapConstraint.h"
#include "Indexes/EdgeIndex.h"
#include "Indexes/FulltextIndex.h"
#include "Indexes/GeoIndex2.h"
#include "Indexes/HashIndex.h"
#include "Indexes/Index.h"
#include "Indexes/PrimaryIndex.h"
#include "Indexes/SkiplistIndex.h"
#include "Utils/transactions.h"
#include "Utils/V8TransactionContext.h"
#include "V8/v8-conv.h"
#include "V8/v8-globals.h"
#include "V8/v8-utils.h"
#include "V8/v8-vpack.h"
#include "V8Server/v8-collection.h"
#include "V8Server/v8-vocbase.h"
#include "V8Server/v8-vocbaseprivate.h"

#include "velocypack/Builder.h"

using namespace std;
using namespace triagens::basics;
using namespace triagens::arango;
using namespace triagens::rest;

////////////////////////////////////////////////////////////////////////////////
/// @brief extract the unique flag from the data
////////////////////////////////////////////////////////////////////////////////

static bool ExtractBoolFlag(v8::Isolate* isolate,
                            v8::Handle<v8::Object> const obj,
                            v8::Handle<v8::String> name, bool defaultValue) {
  // extract unique flag
  if (obj->Has(name)) {
    return TRI_ObjectToBoolean(obj->Get(name));
  }

  return defaultValue;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief checks if argument is an index identifier
////////////////////////////////////////////////////////////////////////////////

static bool IsIndexHandle(v8::Handle<v8::Value> const arg,
                          std::string& collectionName, TRI_idx_iid_t& iid) {
  TRI_ASSERT(collectionName.empty());
  TRI_ASSERT(iid == 0);

  if (arg->IsNumber()) {
    // numeric index id
    iid = (TRI_idx_iid_t)arg->ToNumber()->Value();
    return true;
  }

  if (!arg->IsString()) {
    return false;
  }

  v8::String::Utf8Value str(arg);

  if (*str == nullptr) {
    return false;
  }

  size_t split;
  if (triagens::arango::Index::validateHandle(*str, &split)) {
    collectionName = std::string(*str, split);
    iid = TRI_UInt64String2(*str + split + 1, str.length() - split - 1);
    return true;
  }

  if (triagens::arango::Index::validateId(*str)) {
    iid = TRI_UInt64String2(*str, str.length());
    return true;
  }

  return false;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns the index representation
////////////////////////////////////////////////////////////////////////////////

static v8::Handle<v8::Value> IndexRep(v8::Isolate* isolate,
                                      std::string const& collectionName,
                                      TRI_json_t const* idx) {
  v8::EscapableHandleScope scope(isolate);
  TRI_ASSERT(idx != nullptr);

  v8::Handle<v8::Object> rep = TRI_ObjectJson(isolate, idx)->ToObject();

  std::string iid = TRI_ObjectToString(rep->Get(TRI_V8_ASCII_STRING("id")));
  std::string const id = collectionName + TRI_INDEX_HANDLE_SEPARATOR_STR + iid;
  rep->Set(TRI_V8_ASCII_STRING("id"), TRI_V8_STD_STRING(id));

  return scope.Escape<v8::Value>(rep);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief process the fields list and add them to the json
////////////////////////////////////////////////////////////////////////////////

static int ProcessIndexFields(v8::Isolate* isolate,
                              v8::Handle<v8::Object> const obj,
                              TRI_json_t* json, int numFields, bool create) {
  v8::HandleScope scope(isolate);
  std::set<std::string> fields;

  v8::Handle<v8::String> fieldsString = TRI_V8_ASCII_STRING("fields");
  if (obj->Has(fieldsString) && obj->Get(fieldsString)->IsArray()) {
    // "fields" is a list of fields
    v8::Handle<v8::Array> fieldList =
        v8::Handle<v8::Array>::Cast(obj->Get(fieldsString));

    uint32_t const n = fieldList->Length();

    for (uint32_t i = 0; i < n; ++i) {
      if (!fieldList->Get(i)->IsString()) {
        return TRI_ERROR_BAD_PARAMETER;
      }

      std::string const f = TRI_ObjectToString(fieldList->Get(i));

      if (f.empty() || (create && f[0] == '_')) {
        // accessing internal attributes is disallowed
        return TRI_ERROR_BAD_PARAMETER;
      }

      if (fields.find(f) != fields.end()) {
        // duplicate attribute name
        return TRI_ERROR_BAD_PARAMETER;
      }

      fields.insert(f);
    }
  }

  if (fields.empty() || (numFields > 0 && (int)fields.size() != numFields)) {
    return TRI_ERROR_BAD_PARAMETER;
  }

  TRI_json_t* fieldJson =
      TRI_ObjectToJson(isolate, obj->Get(TRI_V8_ASCII_STRING("fields")));

  if (fieldJson == nullptr) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "fields", fieldJson);

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief process the geojson flag and add it to the json
////////////////////////////////////////////////////////////////////////////////

static int ProcessIndexGeoJsonFlag(v8::Isolate* isolate,
                                   v8::Handle<v8::Object> const obj,
                                   TRI_json_t* json) {
  v8::HandleScope scope(isolate);
  bool geoJson =
      ExtractBoolFlag(isolate, obj, TRI_V8_ASCII_STRING("geoJson"), false);
  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "geoJson",
                        TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, geoJson));

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief process the sparse flag and add it to the json
////////////////////////////////////////////////////////////////////////////////

static int ProcessIndexSparseFlag(v8::Isolate* isolate,
                                  v8::Handle<v8::Object> const obj,
                                  TRI_json_t* json, bool create) {
  v8::HandleScope scope(isolate);
  if (obj->Has(TRI_V8_ASCII_STRING("sparse"))) {
    bool sparse =
        ExtractBoolFlag(isolate, obj, TRI_V8_ASCII_STRING("sparse"), false);
    TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "sparse",
                          TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, sparse));
  } else if (create) {
    // not set. now add a default value
    TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "sparse",
                          TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, false));
  }
  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief process the unique flag and add it to the json
////////////////////////////////////////////////////////////////////////////////

static int ProcessIndexUniqueFlag(v8::Isolate* isolate,
                                  v8::Handle<v8::Object> const obj,
                                  TRI_json_t* json) {
  v8::HandleScope scope(isolate);
  bool unique =
      ExtractBoolFlag(isolate, obj, TRI_V8_ASCII_STRING("unique"), false);
  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "unique",
                        TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, unique));

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of a geo1 index
////////////////////////////////////////////////////////////////////////////////

static int EnhanceJsonIndexGeo1(v8::Isolate* isolate,
                                v8::Handle<v8::Object> const obj,
                                TRI_json_t* json, bool create) {
  int res = ProcessIndexFields(isolate, obj, json, 1, create);
  if (ServerState::instance()->isCoordinator()) {
    TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "ignoreNull",
                          TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, true));
    TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "constraint",
                          TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, false));
  }
  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "sparse",
                        TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, true));
  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "unique",
                        TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, false));
  ProcessIndexGeoJsonFlag(isolate, obj, json);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of a geo2 index
////////////////////////////////////////////////////////////////////////////////

static int EnhanceJsonIndexGeo2(v8::Isolate* isolate,
                                v8::Handle<v8::Object> const obj,
                                TRI_json_t* json, bool create) {
  int res = ProcessIndexFields(isolate, obj, json, 2, create);
  if (ServerState::instance()->isCoordinator()) {
    TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "ignoreNull",
                          TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, true));
    TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "constraint",
                          TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, false));
  }
  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "sparse",
                        TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, true));
  TRI_Insert3ObjectJson(TRI_UNKNOWN_MEM_ZONE, json, "unique",
                        TRI_CreateBooleanJson(TRI_UNKNOWN_MEM_ZONE, false));
  ProcessIndexGeoJsonFlag(isolate, obj, json);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of a hash index
////////////////////////////////////////////////////////////////////////////////

static int EnhanceJsonIndexHash(v8::Isolate* isolate,
                                v8::Handle<v8::Object> const obj,
                                TRI_json_t* json, bool create) {
  int res = ProcessIndexFields(isolate, obj, json, 0, create);
  ProcessIndexSparseFlag(isolate, obj, json, create);
  ProcessIndexUniqueFlag(isolate, obj, json);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of a skiplist index
////////////////////////////////////////////////////////////////////////////////

static int EnhanceJsonIndexSkiplist(v8::Isolate* isolate,
                                    v8::Handle<v8::Object> const obj,
                                    TRI_json_t* json, bool create) {
  int res = ProcessIndexFields(isolate, obj, json, 0, create);
  ProcessIndexSparseFlag(isolate, obj, json, create);
  ProcessIndexUniqueFlag(isolate, obj, json);
  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of a fulltext index
////////////////////////////////////////////////////////////////////////////////

static int EnhanceJsonIndexFulltext(v8::Isolate* isolate,
                                    v8::Handle<v8::Object> const obj,
                                    TRI_json_t* json, bool create) {
  int res = ProcessIndexFields(isolate, obj, json, 1, create);

  // handle "minLength" attribute
  int minWordLength = TRI_FULLTEXT_MIN_WORD_LENGTH_DEFAULT;

  if (obj->Has(TRI_V8_ASCII_STRING("minLength"))) {
    if (obj->Get(TRI_V8_ASCII_STRING("minLength"))->IsNumber() ||
        obj->Get(TRI_V8_ASCII_STRING("minLength"))->IsNumberObject()) {
      minWordLength =
          (int)TRI_ObjectToInt64(obj->Get(TRI_V8_ASCII_STRING("minLength")));
    } else if (!obj->Get(TRI_V8_ASCII_STRING("minLength"))->IsNull() &&
               !obj->Get(TRI_V8_ASCII_STRING("minLength"))->IsUndefined()) {
      return TRI_ERROR_BAD_PARAMETER;
    }
  }
  TRI_Insert3ObjectJson(
      TRI_UNKNOWN_MEM_ZONE, json, "minLength",
      TRI_CreateNumberJson(TRI_UNKNOWN_MEM_ZONE, minWordLength));

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of a cap constraint
////////////////////////////////////////////////////////////////////////////////

static int EnhanceJsonIndexCap(v8::Isolate* isolate,
                               v8::Handle<v8::Object> const obj,
                               TRI_json_t* json) {
  // handle "size" attribute
  size_t count = 0;
  if (obj->Has(TRI_V8_ASCII_STRING("size")) &&
      obj->Get(TRI_V8_ASCII_STRING("size"))->IsNumber()) {
    int64_t value = TRI_ObjectToInt64(obj->Get(TRI_V8_ASCII_STRING("size")));

    if (value < 0 || value > INT64_MAX) {
      return TRI_ERROR_BAD_PARAMETER;
    }
    count = (size_t)value;
  }

  // handle "byteSize" attribute
  int64_t byteSize = 0;
  if (obj->Has(TRI_V8_ASCII_STRING("byteSize")) &&
      obj->Get(TRI_V8_ASCII_STRING("byteSize"))->IsNumber()) {
    byteSize = TRI_ObjectToInt64(obj->Get(TRI_V8_ASCII_STRING("byteSize")));
  }

  if (count == 0 && byteSize <= 0) {
    return TRI_ERROR_BAD_PARAMETER;
  }

  if (byteSize < 0 ||
      (byteSize > 0 && byteSize < triagens::arango::CapConstraint::MinSize)) {
    return TRI_ERROR_BAD_PARAMETER;
  }

  TRI_Insert3ObjectJson(
      TRI_UNKNOWN_MEM_ZONE, json, "size",
      TRI_CreateNumberJson(TRI_UNKNOWN_MEM_ZONE, (double)count));
  TRI_Insert3ObjectJson(
      TRI_UNKNOWN_MEM_ZONE, json, "byteSize",
      TRI_CreateNumberJson(TRI_UNKNOWN_MEM_ZONE, (double)byteSize));

  return TRI_ERROR_NO_ERROR;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief enhances the json of an index
////////////////////////////////////////////////////////////////////////////////

static int EnhanceIndexJson(const v8::FunctionCallbackInfo<v8::Value>& args,
                            TRI_json_t*& json, bool create) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  v8::Handle<v8::Object> obj = args[0].As<v8::Object>();

  // extract index type
  triagens::arango::Index::IndexType type =
      triagens::arango::Index::TRI_IDX_TYPE_UNKNOWN;

  if (obj->Has(TRI_V8_ASCII_STRING("type")) &&
      obj->Get(TRI_V8_ASCII_STRING("type"))->IsString()) {
    TRI_Utf8ValueNFC typeString(TRI_UNKNOWN_MEM_ZONE,
                                obj->Get(TRI_V8_ASCII_STRING("type")));

    if (*typeString == nullptr) {
      return TRI_ERROR_OUT_OF_MEMORY;
    }

    std::string t(*typeString);
    // rewrite type "geo" into either "geo1" or "geo2", depending on the number
    // of fields
    if (t == "geo") {
      t = "geo1";

      if (obj->Has(TRI_V8_ASCII_STRING("fields")) &&
          obj->Get(TRI_V8_ASCII_STRING("fields"))->IsArray()) {
        v8::Handle<v8::Array> f = v8::Handle<v8::Array>::Cast(
            obj->Get(TRI_V8_ASCII_STRING("fields")));
        if (f->Length() == 2) {
          t = "geo2";
        }
      }
    }

    type = triagens::arango::Index::type(t.c_str());
  }

  if (type == triagens::arango::Index::TRI_IDX_TYPE_UNKNOWN) {
    return TRI_ERROR_BAD_PARAMETER;
  }

  if (create) {
    if (type == triagens::arango::Index::TRI_IDX_TYPE_PRIMARY_INDEX ||
        type == triagens::arango::Index::TRI_IDX_TYPE_EDGE_INDEX) {
      // creating these indexes yourself is forbidden
      return TRI_ERROR_FORBIDDEN;
    }
  }

  json = TRI_CreateObjectJson(TRI_UNKNOWN_MEM_ZONE);

  if (json == nullptr) {
    return TRI_ERROR_OUT_OF_MEMORY;
  }

  if (obj->Has(TRI_V8_ASCII_STRING("id"))) {
    uint64_t id = TRI_ObjectToUInt64(obj->Get(TRI_V8_ASCII_STRING("id")), true);
    if (id > 0) {
      char* idString = TRI_StringUInt64(id);
      TRI_Insert3ObjectJson(
          TRI_UNKNOWN_MEM_ZONE, json, "id",
          TRI_CreateStringCopyJson(TRI_UNKNOWN_MEM_ZONE, idString,
                                   strlen(idString)));
      TRI_FreeString(TRI_CORE_MEM_ZONE, idString);
    }
  }

  char const* idxType = triagens::arango::Index::typeName(type);
  TRI_Insert3ObjectJson(
      TRI_UNKNOWN_MEM_ZONE, json, "type",
      TRI_CreateStringCopyJson(TRI_UNKNOWN_MEM_ZONE, idxType, strlen(idxType)));

  int res = TRI_ERROR_INTERNAL;

  switch (type) {
    case triagens::arango::Index::TRI_IDX_TYPE_UNKNOWN:
    case triagens::arango::Index::TRI_IDX_TYPE_PRIORITY_QUEUE_INDEX: {
      res = TRI_ERROR_BAD_PARAMETER;
      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_PRIMARY_INDEX:
    case triagens::arango::Index::TRI_IDX_TYPE_EDGE_INDEX:
    case triagens::arango::Index::TRI_IDX_TYPE_BITARRAY_INDEX: {
      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_GEO1_INDEX:
      res = EnhanceJsonIndexGeo1(isolate, obj, json, create);
      break;

    case triagens::arango::Index::TRI_IDX_TYPE_GEO2_INDEX:
      res = EnhanceJsonIndexGeo2(isolate, obj, json, create);
      break;

    case triagens::arango::Index::TRI_IDX_TYPE_HASH_INDEX:
      res = EnhanceJsonIndexHash(isolate, obj, json, create);
      break;

    case triagens::arango::Index::TRI_IDX_TYPE_SKIPLIST_INDEX:
      res = EnhanceJsonIndexSkiplist(isolate, obj, json, create);
      break;

    case triagens::arango::Index::TRI_IDX_TYPE_FULLTEXT_INDEX:
      res = EnhanceJsonIndexFulltext(isolate, obj, json, create);
      break;

    case triagens::arango::Index::TRI_IDX_TYPE_CAP_CONSTRAINT:
      res = EnhanceJsonIndexCap(isolate, obj, json);
      break;
  }

  return res;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index, coordinator case
////////////////////////////////////////////////////////////////////////////////

static void EnsureIndexCoordinator(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    TRI_vocbase_col_t const* collection, TRI_json_t const* json, bool create) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_ASSERT(collection != nullptr);
  TRI_ASSERT(json != nullptr);

  std::string const databaseName(collection->_dbName);
  std::string const cid = StringUtils::itoa(collection->_cid);
  // TODO: protect against races on _name
  std::string const collectionName(collection->_name);

  TRI_json_t* resultJson = nullptr;
  std::string errorMsg;
  int res = ClusterInfo::instance()->ensureIndexCoordinator(
      databaseName, cid, json, create, &triagens::arango::Index::Compare,
      resultJson, errorMsg, 360.0);

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(res, errorMsg);
  }

  if (resultJson == nullptr) {
    if (!create) {
      // did not find a suitable index
      TRI_V8_RETURN_NULL();
    }

    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  v8::Handle<v8::Value> ret = IndexRep(isolate, collectionName, resultJson);
  TRI_FreeJson(TRI_UNKNOWN_MEM_ZONE, resultJson);

  TRI_V8_RETURN(ret);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index, locally
////////////////////////////////////////////////////////////////////////////////

static void EnsureIndexLocal(const v8::FunctionCallbackInfo<v8::Value>& args,
                             TRI_vocbase_col_t const* collection,
                             TRI_json_t const* json, bool create) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_ASSERT(collection != nullptr);
  TRI_ASSERT(json != nullptr);

  // extract type
  TRI_json_t* value = TRI_LookupObjectJson(json, "type");

  if (!TRI_IsStringJson(value)) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  triagens::arango::Index::IndexType const type =
      triagens::arango::Index::type(value->_value._string.data);

  // extract unique flag
  bool unique = false;
  value = TRI_LookupObjectJson(json, "unique");
  if (TRI_IsBooleanJson(value)) {
    unique = value->_value._boolean;
  }

  // extract sparse flag
  bool sparse = false;
  int sparsity = -1;  // not set
  value = TRI_LookupObjectJson(json, "sparse");
  if (TRI_IsBooleanJson(value)) {
    sparse = value->_value._boolean;
    sparsity = sparse ? 1 : 0;
  }

  // extract id
  TRI_idx_iid_t iid = 0;
  value = TRI_LookupObjectJson(json, "id");
  if (TRI_IsStringJson(value)) {
    iid = TRI_UInt64String2(value->_value._string.data,
                            value->_value._string.length - 1);
  }

  std::vector<std::string> attributes;

  // extract fields
  value = TRI_LookupObjectJson(json, "fields");
  if (TRI_IsArrayJson(value)) {
    // note: "fields" is not mandatory for all index types

    // copy all field names (attributes)
    size_t const n = TRI_LengthArrayJson(value);

    for (size_t i = 0; i < n; ++i) {
      auto v = static_cast<TRI_json_t const*>(
          TRI_AtVector(&value->_value._objects, i));

      if (TRI_IsStringJson(v)) {
        attributes.emplace_back(
            std::string(v->_value._string.data, v->_value._string.length - 1));

        auto last = attributes.back();
        if (last.find("[*]") != std::string::npos) {
          if (type != triagens::arango::Index::TRI_IDX_TYPE_HASH_INDEX &&
              type != triagens::arango::Index::TRI_IDX_TYPE_SKIPLIST_INDEX) {
            // expansion used in index type that does not support it
            TRI_V8_THROW_EXCEPTION_MESSAGE(
                TRI_ERROR_BAD_PARAMETER,
                "cannot use [*] expansion for this type of index");
          } else {
            // expansion used in index type that supports it

            // count number of [*] occurrences
            size_t found = 0;
            size_t offset = 0;

            while ((offset = last.find("[*]", offset)) != std::string::npos) {
              ++found;
              offset += strlen("[*]");
            }

            // only one occurrence is allowed
            if (found > 1) {
              TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_BAD_PARAMETER,
                                             "cannot use multiple [*] "
                                             "expansions for a single index "
                                             "field");
            }
          }
        }
      }
    }
  }

  SingleCollectionReadOnlyTransaction trx(
      new V8TransactionContext(true), collection->_vocbase, collection->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_document_collection_t* document = trx.documentCollection();
  std::string const& collectionName = std::string(collection->_name);

  // disallow index creation in read-only mode
  if (!TRI_IsSystemNameCollection(collectionName.c_str()) && create &&
      TRI_GetOperationModeServer() == TRI_VOCBASE_MODE_NO_CREATE) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_READ_ONLY);
  }

  bool created = false;
  triagens::arango::Index* idx = nullptr;

  switch (type) {
    case triagens::arango::Index::TRI_IDX_TYPE_UNKNOWN:
    case triagens::arango::Index::TRI_IDX_TYPE_PRIMARY_INDEX:
    case triagens::arango::Index::TRI_IDX_TYPE_EDGE_INDEX:
    case triagens::arango::Index::TRI_IDX_TYPE_PRIORITY_QUEUE_INDEX:
    case triagens::arango::Index::TRI_IDX_TYPE_BITARRAY_INDEX: {
      // these indexes cannot be created directly
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
    }

    case triagens::arango::Index::TRI_IDX_TYPE_GEO1_INDEX: {
      if (attributes.size() != 1) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
      }

      bool geoJson = false;
      value = TRI_LookupObjectJson(json, "geoJson");
      if (TRI_IsBooleanJson(value)) {
        geoJson = value->_value._boolean;
      }

      if (create) {
        idx = static_cast<triagens::arango::GeoIndex2*>(
            TRI_EnsureGeoIndex1DocumentCollection(
                &trx, document, iid, attributes[0], geoJson, created));
      } else {
        idx = static_cast<triagens::arango::GeoIndex2*>(
            TRI_LookupGeoIndex1DocumentCollection(document, attributes[0],
                                                  geoJson));
      }
      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_GEO2_INDEX: {
      if (attributes.size() != 2) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
      }

      if (create) {
        idx = static_cast<triagens::arango::GeoIndex2*>(
            TRI_EnsureGeoIndex2DocumentCollection(
                &trx, document, iid, attributes[0], attributes[1], created));
      } else {
        idx = static_cast<triagens::arango::GeoIndex2*>(
            TRI_LookupGeoIndex2DocumentCollection(document, attributes[0],
                                                  attributes[1]));
      }
      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_HASH_INDEX: {
      if (attributes.empty()) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
      }

      if (create) {
        idx = static_cast<triagens::arango::HashIndex*>(
            TRI_EnsureHashIndexDocumentCollection(
                &trx, document, iid, attributes, sparse, unique, created));
      } else {
        idx = static_cast<triagens::arango::HashIndex*>(
            TRI_LookupHashIndexDocumentCollection(document, attributes,
                                                  sparsity, unique));
      }

      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_SKIPLIST_INDEX: {
      if (attributes.empty()) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
      }

      if (create) {
        idx = static_cast<triagens::arango::SkiplistIndex*>(
            TRI_EnsureSkiplistIndexDocumentCollection(
                &trx, document, iid, attributes, sparse, unique, created));
      } else {
        idx = static_cast<triagens::arango::SkiplistIndex*>(
            TRI_LookupSkiplistIndexDocumentCollection(document, attributes,
                                                      sparsity, unique));
      }
      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_FULLTEXT_INDEX: {
      if (attributes.size() != 1) {
        TRI_V8_THROW_EXCEPTION(TRI_ERROR_INTERNAL);
      }

      int minWordLength = TRI_FULLTEXT_MIN_WORD_LENGTH_DEFAULT;
      TRI_json_t const* value = TRI_LookupObjectJson(json, "minLength");
      if (TRI_IsNumberJson(value)) {
        minWordLength = (int)value->_value._number;
      } else if (value != nullptr) {
        // minLength defined but no number
        TRI_V8_THROW_EXCEPTION_PARAMETER("<minLength> must be a number");
      }

      if (create) {
        idx = static_cast<triagens::arango::FulltextIndex*>(
            TRI_EnsureFulltextIndexDocumentCollection(
                &trx, document, iid, attributes[0], minWordLength, created));
      } else {
        idx = static_cast<triagens::arango::FulltextIndex*>(
            TRI_LookupFulltextIndexDocumentCollection(document, attributes[0],
                                                      minWordLength));
      }
      break;
    }

    case triagens::arango::Index::TRI_IDX_TYPE_CAP_CONSTRAINT: {
      size_t size = 0;
      TRI_json_t const* value = TRI_LookupObjectJson(json, "size");
      if (TRI_IsNumberJson(value)) {
        size = (size_t)value->_value._number;
      }

      int64_t byteSize = 0;
      value = TRI_LookupObjectJson(json, "byteSize");
      if (TRI_IsNumberJson(value)) {
        byteSize = (int64_t)value->_value._number;
      }

      if (create) {
        idx = static_cast<triagens::arango::Index*>(
            TRI_EnsureCapConstraintDocumentCollection(&trx, document, iid, size,
                                                      byteSize, created));
      } else {
        idx = static_cast<triagens::arango::Index*>(
            TRI_LookupCapConstraintDocumentCollection(document));
      }
      break;
    }
  }

  if (idx == nullptr && create) {
    // something went wrong during creation
    int res = TRI_errno();

    TRI_V8_THROW_EXCEPTION(res);
  }

  if (idx == nullptr && !create) {
    // no index found
    TRI_V8_RETURN_NULL();
  }

  // found some index to return
  auto indexJson = idx->toJson(TRI_UNKNOWN_MEM_ZONE, false);

  if (indexJson.json() == nullptr) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  v8::Handle<v8::Value> ret =
      IndexRep(isolate, collectionName, indexJson.json());

  if (ret->IsObject()) {
    ret->ToObject()->Set(TRI_V8_ASCII_STRING("isNewlyCreated"),
                         v8::Boolean::New(isolate, create && created));
  }

  TRI_V8_RETURN(ret);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief ensures an index
////////////////////////////////////////////////////////////////////////////////

static void EnsureIndex(const v8::FunctionCallbackInfo<v8::Value>& args,
                        bool create, char const* functionName) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t* collection =
      TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1 || !args[0]->IsObject()) {
    std::string name(functionName);
    name.append("(<description>)");
    TRI_V8_THROW_EXCEPTION_USAGE(name.c_str());
  }

  TRI_json_t* json = nullptr;
  int res = EnhanceIndexJson(args, json, create);

  // this object is responsible for the JSON from now on
  std::unique_ptr<TRI_json_t> jsonDeleter(json);

  if (res == TRI_ERROR_NO_ERROR && ServerState::instance()->isCoordinator()) {
    std::string const dbname(collection->_dbName);
    // TODO: someone might rename the collection while we're reading its name...
    std::string const collname(collection->_name);
    std::shared_ptr<CollectionInfo> c =
        ClusterInfo::instance()->getCollection(dbname, collname);

    if (c->empty()) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
    }

    // check if there is an attempt to create a unique index on non-shard keys
    if (create) {
      TRI_json_t const* v = TRI_LookupObjectJson(json, "unique");

      if (TRI_IsBooleanJson(v) && v->_value._boolean) {
        // unique index, now check if fields and shard keys match
        TRI_json_t const* flds = TRI_LookupObjectJson(json, "fields");

        if (TRI_IsArrayJson(flds) && c->numberOfShards() > 1) {
          std::vector<std::string> const& shardKeys = c->shardKeys();
          size_t const n = TRI_LengthArrayJson(flds);

          if (shardKeys.size() != n) {
            res = TRI_ERROR_CLUSTER_UNSUPPORTED;
          } else {
            for (size_t i = 0; i < n; ++i) {
              TRI_json_t const* f = TRI_LookupArrayJson(flds, i);

              if (!TRI_IsStringJson(f)) {
                res = TRI_ERROR_INTERNAL;
                continue;
              } else {
                if (!TRI_EqualString(f->_value._string.data,
                                     shardKeys[i].c_str())) {
                  res = TRI_ERROR_CLUSTER_UNSUPPORTED;
                }
              }
            }
          }
        }
      }
    }
  }

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_ASSERT(json != nullptr);

  // ensure an index, coordinator case
  if (ServerState::instance()->isCoordinator()) {
    EnsureIndexCoordinator(args, collection, json, create);
  } else {
    EnsureIndexLocal(args, collection, json, create);
  }
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a collection on the coordinator
////////////////////////////////////////////////////////////////////////////////

static void CreateCollectionCoordinator(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    TRI_col_type_e collectionType, std::string const& databaseName,
    VocbaseCollectionInfo& parameters, TRI_vocbase_t* vocbase) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  std::string const name = TRI_ObjectToString(args[0]);

  if (!TRI_IsAllowedNameCollection(parameters.isSystem(), name.c_str())) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_ILLEGAL_NAME);
  }

  bool allowUserKeys = true;
  uint64_t numberOfShards = 1;
  std::vector<std::string> shardKeys;

  // default shard key
  shardKeys.push_back("_key");

  std::string distributeShardsLike;

  std::string cid = "";  // Could come from properties
  if (2 <= args.Length()) {
    if (!args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("<properties> must be an object");
    }

    v8::Handle<v8::Object> p = args[1]->ToObject();

    if (p->Has(TRI_V8_ASCII_STRING("keyOptions")) &&
        p->Get(TRI_V8_ASCII_STRING("keyOptions"))->IsObject()) {
      v8::Handle<v8::Object> o = v8::Handle<v8::Object>::Cast(
          p->Get(TRI_V8_ASCII_STRING("keyOptions")));

      if (o->Has(TRI_V8_ASCII_STRING("type"))) {
        std::string const type =
            TRI_ObjectToString(o->Get(TRI_V8_ASCII_STRING("type")));

        if (type != "" && type != "traditional") {
          // invalid key generator
          TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_CLUSTER_UNSUPPORTED,
                                         "non-traditional key generators are "
                                         "not supported for sharded "
                                         "collections");
        }
      }

      if (o->Has(TRI_V8_ASCII_STRING("allowUserKeys"))) {
        allowUserKeys =
            TRI_ObjectToBoolean(o->Get(TRI_V8_ASCII_STRING("allowUserKeys")));
      }
    }

    if (p->Has(TRI_V8_ASCII_STRING("numberOfShards"))) {
      numberOfShards = TRI_ObjectToUInt64(
          p->Get(TRI_V8_ASCII_STRING("numberOfShards")), false);
    }

    if (p->Has(TRI_V8_ASCII_STRING("shardKeys"))) {
      shardKeys.clear();

      if (p->Get(TRI_V8_ASCII_STRING("shardKeys"))->IsArray()) {
        v8::Handle<v8::Array> k = v8::Handle<v8::Array>::Cast(
            p->Get(TRI_V8_ASCII_STRING("shardKeys")));

        for (uint32_t i = 0; i < k->Length(); ++i) {
          v8::Handle<v8::Value> v = k->Get(i);
          if (v->IsString()) {
            std::string const key = TRI_ObjectToString(v);

            // system attributes are not allowed (except _key)
            if (!key.empty() && (key[0] != '_' || key == "_key")) {
              shardKeys.push_back(key);
            }
          }
        }
      }
    }

    if (p->Has(TRI_V8_ASCII_STRING("distributeShardsLike")) &&
        p->Get(TRI_V8_ASCII_STRING("distributeShardsLike"))->IsString()) {
      distributeShardsLike = TRI_ObjectToString(
          p->Get(TRI_V8_ASCII_STRING("distributeShardsLike")));
    }

    auto idKey = TRI_V8_ASCII_STRING("id");
    if (p->Has(idKey) && p->Get(idKey)->IsString()) {
      cid = TRI_ObjectToString(p->Get(idKey));
    }
  }

  if (numberOfShards == 0 || numberOfShards > 1000) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("invalid number of shards");
  }

  if (shardKeys.empty() || shardKeys.size() > 8) {
    TRI_V8_THROW_EXCEPTION_PARAMETER("invalid number of shard keys");
  }

  ClusterInfo* ci = ClusterInfo::instance();

  // fetch a unique id for the new collection plus one for each shard to create
  uint64_t const id = ci->uniqid(1 + numberOfShards);
  if (cid.empty()) {
    // collection id is the first unique id we got
    cid = StringUtils::itoa(id);
    // if id was given, the first unique id is wasted, this does not matter
  }

  std::vector<std::string> dbServers;

  if (distributeShardsLike.empty()) {
    // fetch list of available servers in cluster, and shuffle them randomly
    dbServers = ci->getCurrentDBServers();

    if (dbServers.empty()) {
      TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_ERROR_INTERNAL,
                                     "no database servers found in cluster");
    }

    random_shuffle(dbServers.begin(), dbServers.end());
  } else {
    CollectionNameResolver resolver(vocbase);
    TRI_voc_cid_t otherCid =
        resolver.getCollectionIdCluster(distributeShardsLike);
    std::string otherCidString = triagens::basics::StringUtils::itoa(otherCid);
    std::shared_ptr<CollectionInfo> collInfo =
        ci->getCollection(databaseName, otherCidString);
    auto shards = collInfo->shardIds();
    auto shardList = ci->getShardList(otherCidString);
    for (auto const& s : *shardList) {
      auto it = shards->find(s);
      if (it != shards->end()) {
        dbServers.push_back(it->second[0]);
      }
    }
  }

  // now create the shards
  std::map<std::string, std::string> shards;
  for (uint64_t i = 0; i < numberOfShards; ++i) {
    // determine responsible server
    std::string serverId = dbServers[i % dbServers.size()];

    // determine shard id
    std::string shardId = "s" + StringUtils::itoa(id + 1 + i);

    shards.insert(std::make_pair(shardId, serverId));
  }

  // now create the VelocyPack for the collection
  arangodb::velocypack::Builder velocy;
  using arangodb::velocypack::Value;
  using arangodb::velocypack::ValueType;
  using arangodb::velocypack::ObjectBuilder;
  using arangodb::velocypack::ArrayBuilder;

  {
    ObjectBuilder ob(&velocy);
    velocy("id", Value(cid))("name", Value(name))("type",
                                                  Value((int)collectionType))(
        "status", Value((int)TRI_VOC_COL_STATUS_LOADED))(
        "deleted", Value(parameters.deleted()))("doCompact",
                                                Value(parameters.doCompact()))(
        "isSystem", Value(parameters.isSystem()))(
        "isVolatile", Value(parameters.isVolatile()))(
        "waitForSync", Value(parameters.waitForSync()))(
        "journalSize", Value(parameters.maximalSize()))(
        "indexBuckets", Value(parameters.indexBuckets()))(
        "keyOptions", Value(ValueType::Object))("type", Value("traditional"))(
        "allowUserKeys", Value(allowUserKeys)).close();

    {
      ArrayBuilder ab(&velocy, "shardKeys");
      for (auto const& sk : shardKeys) {
        velocy(Value(sk));
      }
    }

    {
      ObjectBuilder ob(&velocy, "shards");
      for (auto const& p : shards) {
        ArrayBuilder ab(&velocy, p.first);
        velocy(Value(p.second));
      }
    }

    {
      ArrayBuilder ab(&velocy, "indexes");

      // create a dummy primary index
      TRI_document_collection_t* doc = nullptr;
      std::unique_ptr<triagens::arango::PrimaryIndex> primaryIndex(
          new triagens::arango::PrimaryIndex(doc));

      auto idxJson = primaryIndex->toJson(TRI_UNKNOWN_MEM_ZONE, false);
      triagens::basics::JsonHelper::toVelocyPack(idxJson.json(), velocy);

      if (collectionType == TRI_COL_TYPE_EDGE) {
        // create a dummy edge index
        auto edgeIndex =
            std::make_unique<triagens::arango::EdgeIndex>(id, nullptr);

        idxJson = edgeIndex->toJson(TRI_UNKNOWN_MEM_ZONE, false);
        triagens::basics::JsonHelper::toVelocyPack(idxJson.json(), velocy);
      }
    }
  }

  std::string errorMsg;
  int myerrno = ci->createCollectionCoordinator(
      databaseName, cid, numberOfShards, velocy.slice(), errorMsg, 240.0);

  if (myerrno != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(myerrno, errorMsg);
  }
  ci->loadPlannedCollections();

  std::shared_ptr<CollectionInfo> c = ci->getCollection(databaseName, cid);
  TRI_vocbase_col_t* newcoll = CoordinatorCollection(vocbase, *c);
  TRI_V8_RETURN(WrapCollection(isolate, newcoll));
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionEnsureIndex
////////////////////////////////////////////////////////////////////////////////

static void JS_EnsureIndexVocbaseCol(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  PREVENT_EMBEDDED_TRANSACTION();

  EnsureIndex(args, true, "ensureIndex");
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an index
////////////////////////////////////////////////////////////////////////////////

static void JS_LookupIndexVocbaseCol(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  EnsureIndex(args, false, "lookupIndex");
  TRI_V8_TRY_CATCH_END
}
////////////////////////////////////////////////////////////////////////////////
/// @brief drops an index, coordinator case
////////////////////////////////////////////////////////////////////////////////

static void DropIndexCoordinator(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    TRI_vocbase_col_t const* collection, v8::Handle<v8::Value> const val) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  std::string collectionName;
  TRI_idx_iid_t iid = 0;

  // extract the index identifier from a string
  if (val->IsString() || val->IsStringObject() || val->IsNumber()) {
    if (!IsIndexHandle(val, collectionName, iid)) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
    }
  }

  // extract the index identifier from an object
  else if (val->IsObject()) {
    TRI_GET_GLOBALS();

    v8::Handle<v8::Object> obj = val->ToObject();
    TRI_GET_GLOBAL_STRING(IdKey);
    v8::Handle<v8::Value> iidVal = obj->Get(IdKey);

    if (!IsIndexHandle(iidVal, collectionName, iid)) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
    }
  }

  if (!collectionName.empty()) {
    CollectionNameResolver resolver(collection->_vocbase);

    if (!EqualCollection(&resolver, collectionName, collection)) {
      TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
    }
  }

  std::string const databaseName(collection->_dbName);
  std::string const cid = StringUtils::itoa(collection->_cid);
  std::string errorMsg;

  int res = ClusterInfo::instance()->dropIndexCoordinator(databaseName, cid,
                                                          iid, errorMsg, 0.0);

  if (res == TRI_ERROR_NO_ERROR) {
    TRI_V8_RETURN_TRUE();
  }
  TRI_V8_RETURN_FALSE();
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock col_dropIndex
////////////////////////////////////////////////////////////////////////////////

static void JS_DropIndexVocbaseCol(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  PREVENT_EMBEDDED_TRANSACTION();

  TRI_vocbase_col_t* collection =
      TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (args.Length() != 1) {
    TRI_V8_THROW_EXCEPTION_USAGE("dropIndex(<index-handle>)");
  }

  if (ServerState::instance()->isCoordinator()) {
    DropIndexCoordinator(args, collection, args[0]);
    return;
  }

  SingleCollectionReadOnlyTransaction trx(
      new V8TransactionContext(true), collection->_vocbase, collection->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  TRI_document_collection_t* document = trx.documentCollection();

  auto idx = TRI_LookupIndexByHandle(isolate, trx.resolver(), collection,
                                     args[0], true);

  if (idx == nullptr) {
    return;
  }

  if (idx->id() == 0) {
    TRI_V8_RETURN_FALSE();
  }

  if (idx->type() == triagens::arango::Index::TRI_IDX_TYPE_PRIMARY_INDEX ||
      idx->type() == triagens::arango::Index::TRI_IDX_TYPE_EDGE_INDEX) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_FORBIDDEN);
  }

  // .............................................................................
  // inside a write transaction, write-lock is acquired by TRI_DropIndex...
  // .............................................................................

  bool ok = TRI_DropIndexDocumentCollection(document, idx->id(), true);

  // .............................................................................
  // outside a write transaction
  // .............................................................................

  if (ok) {
    TRI_V8_RETURN_TRUE();
  }

  TRI_V8_RETURN_FALSE();
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief returns information about the indexes, coordinator case
////////////////////////////////////////////////////////////////////////////////

static void GetIndexesCoordinator(
    const v8::FunctionCallbackInfo<v8::Value>& args,
    TRI_vocbase_col_t const* collection) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  std::string const databaseName(collection->_dbName);
  std::string const cid = StringUtils::itoa(collection->_cid);
  std::string const collectionName(collection->_name);

  std::shared_ptr<CollectionInfo> c =
      ClusterInfo::instance()->getCollection(databaseName, cid);

  if ((*c).empty()) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_COLLECTION_NOT_FOUND);
  }

  v8::Handle<v8::Array> ret = v8::Array::New(isolate);

  TRI_json_t const* json = (*c).getIndexes();

  if (TRI_IsArrayJson(json)) {
    uint32_t j = 0;
    size_t const n = TRI_LengthArrayJson(json);

    for (size_t i = 0; i < n; ++i) {
      TRI_json_t const* v = TRI_LookupArrayJson(json, i);

      if (v != nullptr) {
        ret->Set(j++, IndexRep(isolate, collectionName, v));
      }
    }
  }

  TRI_V8_RETURN(ret);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionGetIndexes
////////////////////////////////////////////////////////////////////////////////

static void JS_GetIndexesVocbaseCol(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  v8::HandleScope scope(isolate);

  TRI_vocbase_col_t* collection =
      TRI_UnwrapClass<TRI_vocbase_col_t>(args.Holder(), WRP_VOCBASE_COL_TYPE);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_INTERNAL("cannot extract collection");
  }

  if (ServerState::instance()->isCoordinator()) {
    GetIndexesCoordinator(args, collection);
    return;
  }

  bool withFigures = false;
  if (args.Length() > 0) {
    withFigures = TRI_ObjectToBoolean(args[0]);
  }

  SingleCollectionReadOnlyTransaction trx(
      new V8TransactionContext(true), collection->_vocbase, collection->_cid);

  int res = trx.begin();

  if (res != TRI_ERROR_NO_ERROR) {
    TRI_V8_THROW_EXCEPTION(res);
  }

  // READ-LOCK start
  trx.lockRead();

  TRI_document_collection_t* document = trx.documentCollection();
  std::string const& collectionName = std::string(collection->_name);

  // get list of indexes
  auto&& indexes = TRI_IndexesDocumentCollection(document, withFigures);

  trx.finish(res);
  // READ-LOCK end

  size_t const n = indexes.size();
  v8::Handle<v8::Array> result = v8::Array::New(isolate, static_cast<int>(n));

  for (size_t i = 0; i < n; ++i) {
    auto const& idx = indexes[i];

    result->Set(static_cast<uint32_t>(i),
                IndexRep(isolate, collectionName, idx.json()));
  }

  TRI_V8_RETURN(result);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief looks up an index identifier
////////////////////////////////////////////////////////////////////////////////

triagens::arango::Index* TRI_LookupIndexByHandle(
    v8::Isolate* isolate,
    triagens::arango::CollectionNameResolver const* resolver,
    TRI_vocbase_col_t const* collection, v8::Handle<v8::Value> const val,
    bool ignoreNotFound) {
  // reset the collection identifier
  std::string collectionName;
  TRI_idx_iid_t iid = 0;

  // assume we are already loaded
  TRI_ASSERT(collection != nullptr);
  TRI_ASSERT(collection->_collection != nullptr);

  // extract the index identifier from a string
  if (val->IsString() || val->IsStringObject() || val->IsNumber()) {
    if (!IsIndexHandle(val, collectionName, iid)) {
      TRI_V8_SET_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
      return nullptr;
    }
  }

  // extract the index identifier from an object
  else if (val->IsObject()) {
    TRI_GET_GLOBALS();

    v8::Handle<v8::Object> obj = val->ToObject();
    TRI_GET_GLOBAL_STRING(IdKey);
    v8::Handle<v8::Value> iidVal = obj->Get(IdKey);

    if (!IsIndexHandle(iidVal, collectionName, iid)) {
      TRI_V8_SET_EXCEPTION(TRI_ERROR_ARANGO_INDEX_HANDLE_BAD);
      return nullptr;
    }
  }

  if (!collectionName.empty()) {
    if (!EqualCollection(resolver, collectionName, collection)) {
      // I wish this error provided me with more information!
      // e.g. 'cannot access index outside the collection it was defined in'
      TRI_V8_SET_EXCEPTION(TRI_ERROR_ARANGO_CROSS_COLLECTION_REQUEST);
      return nullptr;
    }
  }

  auto idx = collection->_collection->lookupIndex(iid);

  if (idx == nullptr) {
    if (!ignoreNotFound) {
      TRI_V8_SET_EXCEPTION(TRI_ERROR_ARANGO_INDEX_NOT_FOUND);
      return nullptr;
    }
  }

  return idx;
}

////////////////////////////////////////////////////////////////////////////////
/// @brief create a collection
////////////////////////////////////////////////////////////////////////////////

static void CreateVocBase(const v8::FunctionCallbackInfo<v8::Value>& args,
                          TRI_col_type_e collectionType) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::HandleScope scope(isolate);

  TRI_vocbase_t* vocbase = GetContextVocBase(isolate);

  if (vocbase == nullptr) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_DATABASE_NOT_FOUND);
  }

  // ...........................................................................
  // We require exactly 1 or exactly 2 arguments -- anything else is an error
  // ...........................................................................

  if (args.Length() < 1 || args.Length() > 3) {
    TRI_V8_THROW_EXCEPTION_USAGE("_create(<name>, <properties>, <type>)");
  }

  if (TRI_GetOperationModeServer() == TRI_VOCBASE_MODE_NO_CREATE) {
    TRI_V8_THROW_EXCEPTION(TRI_ERROR_ARANGO_READ_ONLY);
  }

  // optional, third parameter can override collection type
  if (args.Length() == 3 && args[2]->IsString()) {
    std::string typeString = TRI_ObjectToString(args[2]);
    if (typeString == "edge") {
      collectionType = TRI_COL_TYPE_EDGE;
    } else if (typeString == "document") {
      collectionType = TRI_COL_TYPE_DOCUMENT;
    }
  }

  PREVENT_EMBEDDED_TRANSACTION();

  // extract the name
  std::string const name = TRI_ObjectToString(args[0]);

  VPackBuilder builder;
  VPackSlice infoSlice;
  if (2 <= args.Length()) {
    if (!args[1]->IsObject()) {
      TRI_V8_THROW_TYPE_ERROR("<properties> must be an object");
    }

    int res = TRI_V8ToVPack(isolate, builder, args[1], false);

    if (res != TRI_ERROR_NO_ERROR) {
      TRI_V8_THROW_EXCEPTION(res);
    }

    infoSlice = builder.slice();
  }

  VocbaseCollectionInfo parameters(vocbase, name.c_str(), collectionType,
                                   infoSlice);

  if (ServerState::instance()->isCoordinator()) {
    CreateCollectionCoordinator(args, collectionType, vocbase->_name,
                                parameters, vocbase);
    return;
  }

  TRI_vocbase_col_t const* collection =
      TRI_CreateCollectionVocBase(vocbase, parameters, parameters.id(), true);

  if (collection == nullptr) {
    TRI_V8_THROW_EXCEPTION_MESSAGE(TRI_errno(), "cannot create collection");
  }

  v8::Handle<v8::Value> result = WrapCollection(isolate, collection);

  if (result.IsEmpty()) {
    TRI_V8_THROW_EXCEPTION_MEMORY();
  }

  TRI_V8_RETURN(result);
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionDatabaseCreate
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateVocbase(const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_DOCUMENT);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionCreateDocumentCollection
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateDocumentCollectionVocbase(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_DOCUMENT);
  TRI_V8_TRY_CATCH_END
}

////////////////////////////////////////////////////////////////////////////////
/// @brief was docuBlock collectionCreateEdgeCollection
////////////////////////////////////////////////////////////////////////////////

static void JS_CreateEdgeCollectionVocbase(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  TRI_V8_TRY_CATCH_BEGIN(isolate);
  CreateVocBase(args, TRI_COL_TYPE_EDGE);
  TRI_V8_TRY_CATCH_END
}

void TRI_InitV8indexArangoDB(v8::Isolate* isolate,
                             v8::Handle<v8::ObjectTemplate> rt) {
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("_create"),
                       JS_CreateVocbase, true);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING("_createEdgeCollection"),
                       JS_CreateEdgeCollectionVocbase);
  TRI_AddMethodVocbase(isolate, rt,
                       TRI_V8_ASCII_STRING("_createDocumentCollection"),
                       JS_CreateDocumentCollectionVocbase);
}

void TRI_InitV8indexCollection(v8::Isolate* isolate,
                               v8::Handle<v8::ObjectTemplate> rt) {
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("dropIndex"),
                       JS_DropIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("ensureIndex"),
                       JS_EnsureIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("lookupIndex"),
                       JS_LookupIndexVocbaseCol);
  TRI_AddMethodVocbase(isolate, rt, TRI_V8_ASCII_STRING("getIndexes"),
                       JS_GetIndexesVocbaseCol);
}
