#pragma once

#include <Database/Database.hxx>
#include <SQL/SQL.hxx>
#include <string>

namespace AstralDB {
namespace SQL {

/** Filter \p SourceTable rows in place to those participating in \p Spec pattern matches. */
void RunMatchRecognize(Database &Db, const std::string &SourceTable, const MatchRecognizeSpec &Spec);

} // namespace SQL
} // namespace AstralDB
