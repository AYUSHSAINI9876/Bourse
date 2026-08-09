#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "bourse/sql/engine.hpp"
#include "bourse/sql/lexer.hpp"
#include "bourse/sql/parser.hpp"

using namespace bourse;
using namespace bourse::sql;

namespace {

class SqlFixture : public ::testing::Test {
 protected:
  ResultSet run(std::string_view statement) {
    Result<ResultSet> result = engine_.execute(statement);
    EXPECT_TRUE(result.ok()) << statement << " -> " << result.status().toString();
    return result.ok() ? std::move(result).value() : ResultSet{};
  }

  Status runExpectingFailure(std::string_view statement) {
    Result<ResultSet> result = engine_.execute(statement);
    EXPECT_FALSE(result.ok()) << statement << " unexpectedly succeeded";
    return result.ok() ? Status::success() : result.status();
  }

  void seed() {
    run("CREATE TABLE trades (id INTEGER PRIMARY KEY, symbol TEXT, qty INTEGER, price REAL)");
    run("INSERT INTO trades VALUES (1, 'AAPL', 100, 150.25)");
    run("INSERT INTO trades VALUES (2, 'MSFT', 50, 300.10)");
    run("INSERT INTO trades VALUES (3, 'AAPL', 75, 151.00)");
    run("INSERT INTO trades VALUES (4, 'TSLA', 200, 210.50)");
  }

  Engine engine_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

TEST(Lexer, TokenizesAStatement) {
  Lexer lexer("SELECT id FROM t WHERE x >= 10");
  Result<std::vector<Token>> tokens = lexer.tokenize();
  ASSERT_TRUE(tokens.ok());
  ASSERT_GE(tokens.value().size(), 8u);
  EXPECT_EQ(tokens.value()[0].type, TokenType::kKeyword);
  EXPECT_EQ(tokens.value()[0].upper, "SELECT");
  EXPECT_EQ(tokens.value()[1].type, TokenType::kIdentifier);
  // SELECT id FROM t WHERE x >= 10  ->  the operator is token 6.
  EXPECT_TRUE(tokens.value()[6].isOperator(">="));
  EXPECT_EQ(tokens.value()[7].type, TokenType::kNumber);
  EXPECT_EQ(tokens.value().back().type, TokenType::kEndOfInput);
}

TEST(Lexer, HandlesDoubledQuoteEscapes) {
  Lexer lexer("SELECT 'it''s here'");
  Result<std::vector<Token>> tokens = lexer.tokenize();
  ASSERT_TRUE(tokens.ok());
  EXPECT_EQ(tokens.value()[1].type, TokenType::kString);
  EXPECT_EQ(tokens.value()[1].text, "it's here");
}

TEST(Lexer, SkipsComments) {
  Lexer lexer("SELECT 1 -- trailing\n/* block */ FROM t");
  Result<std::vector<Token>> tokens = lexer.tokenize();
  ASSERT_TRUE(tokens.ok());
  bool saw_from = false;
  for (const Token& token : tokens.value()) {
    if (token.isKeyword("FROM")) {
      saw_from = true;
    }
  }
  EXPECT_TRUE(saw_from);
}

TEST(Lexer, ReportsUnterminatedStringsWithAPosition) {
  Lexer lexer("SELECT 'oops");
  Result<std::vector<Token>> tokens = lexer.tokenize();
  ASSERT_FALSE(tokens.ok());
  EXPECT_NE(tokens.status().message().find("position"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Parser and the Visitor
// ---------------------------------------------------------------------------

TEST(Parser, RespectsOperatorPrecedence) {
  // The printing visitor fully parenthesises, so precedence is directly
  // observable rather than inferred.
  Result<Statement> parsed = Parser::parse("SELECT a FROM t WHERE a + b * c > 10");
  ASSERT_TRUE(parsed.ok()) << parsed.status().toString();
  const auto* select = std::get_if<SelectStatement>(&parsed.value());
  ASSERT_NE(select, nullptr);
  ASSERT_NE(select->where, nullptr);
  EXPECT_EQ(expressionToString(*select->where), "((a + (b * c)) > 10)");
}

TEST(Parser, AndBindsTighterThanOr) {
  Result<Statement> parsed = Parser::parse("SELECT a FROM t WHERE a = 1 OR b = 2 AND c = 3");
  ASSERT_TRUE(parsed.ok());
  const auto* select = std::get_if<SelectStatement>(&parsed.value());
  ASSERT_NE(select, nullptr);
  EXPECT_EQ(expressionToString(*select->where), "((a = 1) OR ((b = 2) AND (c = 3)))");
}

TEST(Parser, ParenthesesOverridePrecedence) {
  Result<Statement> parsed = Parser::parse("SELECT a FROM t WHERE (a + b) * c > 10");
  ASSERT_TRUE(parsed.ok());
  const auto* select = std::get_if<SelectStatement>(&parsed.value());
  EXPECT_EQ(expressionToString(*select->where), "(((a + b) * c) > 10)");
}

TEST(Parser, ParsesIsNullAsPostfix) {
  Result<Statement> parsed = Parser::parse("SELECT a FROM t WHERE a IS NOT NULL");
  ASSERT_TRUE(parsed.ok());
  const auto* select = std::get_if<SelectStatement>(&parsed.value());
  EXPECT_EQ(expressionToString(*select->where), "(a IS NOT NULL)");
}

TEST(Parser, ReportsUsefulErrors) {
  Result<Statement> missing_from = Parser::parse("SELECT a b c");
  ASSERT_FALSE(missing_from.ok());
  EXPECT_NE(missing_from.status().message().find("expected"), std::string::npos);

  EXPECT_FALSE(Parser::parse("SELECT FROM").ok());
  EXPECT_FALSE(Parser::parse("INSERT INTO t VALUES").ok());
  EXPECT_FALSE(Parser::parse("").ok());
  EXPECT_FALSE(Parser::parse("SELECT * FROM t EXTRA").ok());
}

// ---------------------------------------------------------------------------
// DDL
// ---------------------------------------------------------------------------

TEST_F(SqlFixture, CreatesAndDropsTables) {
  run("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT)");
  EXPECT_TRUE(engine_.hasTable("users"));
  EXPECT_TRUE(engine_.hasTable("USERS")) << "table names must be case-insensitive";

  const std::optional<TableSchema> schema = engine_.schemaOf("users");
  ASSERT_TRUE(schema.has_value());
  ASSERT_EQ(schema->columns.size(), 2u);
  EXPECT_TRUE(schema->columns[0].primary_key);
  EXPECT_TRUE(schema->columns[0].not_null) << "a primary key is implicitly NOT NULL";

  run("DROP TABLE users");
  EXPECT_FALSE(engine_.hasTable("users"));
}

TEST_F(SqlFixture, RejectsDuplicateTablesAndColumns) {
  run("CREATE TABLE t (a INTEGER)");
  EXPECT_EQ(runExpectingFailure("CREATE TABLE t (a INTEGER)").code(), ErrorCode::kAlreadyExists);
  // IF NOT EXISTS makes it a no-op instead.
  EXPECT_TRUE(engine_.execute("CREATE TABLE IF NOT EXISTS t (a INTEGER)").ok());
  EXPECT_FALSE(engine_.execute("CREATE TABLE u (a INTEGER, a TEXT)").ok());
}

// ---------------------------------------------------------------------------
// DML
// ---------------------------------------------------------------------------

TEST_F(SqlFixture, InsertsAndSelects) {
  seed();
  const ResultSet all = run("SELECT * FROM trades");
  EXPECT_EQ(all.columns.size(), 4u);
  EXPECT_EQ(all.rows.size(), 4u);
  EXPECT_EQ(engine_.rowCount("trades"), 4u);
}

TEST_F(SqlFixture, EnforcesPrimaryKeyUniqueness) {
  seed();
  const Status duplicate = runExpectingFailure("INSERT INTO trades VALUES (1, 'DUP', 1, 1.0)");
  EXPECT_EQ(duplicate.code(), ErrorCode::kAlreadyExists);
  EXPECT_EQ(engine_.rowCount("trades"), 4u) << "a rejected insert must not change the table";
}

TEST_F(SqlFixture, EnforcesNotNull) {
  run("CREATE TABLE t (id INTEGER NOT NULL, note TEXT)");
  EXPECT_FALSE(engine_.execute("INSERT INTO t VALUES (NULL, 'x')").ok());
  EXPECT_TRUE(engine_.execute("INSERT INTO t VALUES (1, NULL)").ok());
}

TEST_F(SqlFixture, InsertIsAllOrNothing) {
  seed();
  // The second row collides on the primary key, so neither should land.
  EXPECT_FALSE(engine_.execute("INSERT INTO trades VALUES (9, 'A', 1, 1.0), (1, 'B', 1, 1.0)").ok());
  EXPECT_EQ(engine_.rowCount("trades"), 4u) << "a partially-failing INSERT left rows behind";
}

TEST_F(SqlFixture, InsertsNamedColumnsInAnyOrder) {
  run("CREATE TABLE t (a INTEGER, b TEXT, c INTEGER)");
  run("INSERT INTO t (c, a) VALUES (3, 1)");
  const ResultSet rows = run("SELECT a, b, c FROM t");
  ASSERT_EQ(rows.rows.size(), 1u);
  EXPECT_EQ(rows.rows[0][0].asInteger(), 1);
  EXPECT_TRUE(rows.rows[0][1].isNull()) << "unmentioned columns must default to NULL";
  EXPECT_EQ(rows.rows[0][2].asInteger(), 3);
}

TEST_F(SqlFixture, FiltersWithWhere) {
  seed();
  EXPECT_EQ(run("SELECT * FROM trades WHERE symbol = 'AAPL'").rows.size(), 2u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE qty > 80").rows.size(), 2u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE qty > 80 AND symbol = 'AAPL'").rows.size(), 1u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE symbol = 'AAPL' OR symbol = 'TSLA'").rows.size(), 3u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE NOT symbol = 'AAPL'").rows.size(), 2u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE symbol LIKE 'A%'").rows.size(), 2u);
}

TEST_F(SqlFixture, ComparingWithNullMatchesNothing) {
  run("CREATE TABLE t (a INTEGER, b TEXT)");
  run("INSERT INTO t VALUES (1, 'x'), (2, NULL)");
  // The standard, surprising result: `= NULL` never matches.
  EXPECT_EQ(run("SELECT * FROM t WHERE b = NULL").rows.size(), 0u);
  EXPECT_EQ(run("SELECT * FROM t WHERE b IS NULL").rows.size(), 1u);
  EXPECT_EQ(run("SELECT * FROM t WHERE b IS NOT NULL").rows.size(), 1u);
}

TEST_F(SqlFixture, OrdersAndLimits) {
  seed();
  const ResultSet ascending = run("SELECT qty FROM trades ORDER BY qty");
  ASSERT_EQ(ascending.rows.size(), 4u);
  EXPECT_EQ(ascending.rows[0][0].asInteger(), 50);
  EXPECT_EQ(ascending.rows[3][0].asInteger(), 200);

  const ResultSet descending = run("SELECT qty FROM trades ORDER BY qty DESC");
  EXPECT_EQ(descending.rows[0][0].asInteger(), 200);

  EXPECT_EQ(run("SELECT * FROM trades ORDER BY qty LIMIT 2").rows.size(), 2u);
  const ResultSet paged = run("SELECT qty FROM trades ORDER BY qty LIMIT 2 OFFSET 1");
  ASSERT_EQ(paged.rows.size(), 2u);
  EXPECT_EQ(paged.rows[0][0].asInteger(), 75);
}

TEST_F(SqlFixture, OrdersNullsFirst) {
  run("CREATE TABLE t (a INTEGER)");
  run("INSERT INTO t VALUES (2), (NULL), (1)");
  const ResultSet sorted = run("SELECT a FROM t ORDER BY a");
  ASSERT_EQ(sorted.rows.size(), 3u);
  EXPECT_TRUE(sorted.rows[0][0].isNull()) << "ORDER BY needs a total order; NULL sorts first";
}

TEST_F(SqlFixture, ProjectsExpressions) {
  seed();
  const ResultSet computed = run("SELECT qty * 2 AS doubled FROM trades WHERE id = 1");
  ASSERT_EQ(computed.rows.size(), 1u);
  EXPECT_EQ(computed.columns[0], "doubled");
  EXPECT_EQ(computed.rows[0][0].asInteger(), 200);

  const ResultSet unaliased = run("SELECT qty + 1 FROM trades WHERE id = 1");
  EXPECT_EQ(unaliased.columns[0], "(qty + 1)") << "an unaliased expression is named by the printer";
}

TEST_F(SqlFixture, Updates) {
  seed();
  const ResultSet updated = run("UPDATE trades SET qty = 999 WHERE symbol = 'AAPL'");
  EXPECT_EQ(updated.rows_affected, 2u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE qty = 999").rows.size(), 2u);

  run("UPDATE trades SET qty = qty + 1 WHERE id = 2");
  const ResultSet incremented = run("SELECT qty FROM trades WHERE id = 2");
  EXPECT_EQ(incremented.rows[0][0].asInteger(), 51);
}

TEST_F(SqlFixture, UpdateEvaluatesAgainstThePreUpdateRow) {
  run("CREATE TABLE t (a INTEGER, b INTEGER)");
  run("INSERT INTO t VALUES (1, 2)");
  // A swap only works if the right-hand sides see the original values.
  run("UPDATE t SET a = b, b = a");
  const ResultSet swapped = run("SELECT a, b FROM t");
  EXPECT_EQ(swapped.rows[0][0].asInteger(), 2);
  EXPECT_EQ(swapped.rows[0][1].asInteger(), 1);
}

TEST_F(SqlFixture, Deletes) {
  seed();
  EXPECT_EQ(run("DELETE FROM trades WHERE symbol = 'AAPL'").rows_affected, 2u);
  EXPECT_EQ(engine_.rowCount("trades"), 2u);
  EXPECT_EQ(run("DELETE FROM trades").rows_affected, 2u);
  EXPECT_EQ(engine_.rowCount("trades"), 0u);
}

// ---------------------------------------------------------------------------
// Types
// ---------------------------------------------------------------------------

TEST_F(SqlFixture, CoercesOnInsertAndRejectsImpossibleValues) {
  run("CREATE TABLE t (n INTEGER, r REAL, s TEXT)");
  run("INSERT INTO t VALUES ('42', '1.5', 7)");
  const ResultSet rows = run("SELECT n, r, s FROM t");
  EXPECT_EQ(rows.rows[0][0].type(), DatumType::kInteger);
  EXPECT_EQ(rows.rows[0][0].asInteger(), 42);
  EXPECT_EQ(rows.rows[0][1].type(), DatumType::kReal);
  EXPECT_EQ(rows.rows[0][2].type(), DatumType::kText);

  EXPECT_FALSE(engine_.execute("INSERT INTO t VALUES ('not a number', 1.0, 'x')").ok());
}

TEST_F(SqlFixture, ReportsErrorsInsteadOfCrashing) {
  seed();
  EXPECT_EQ(runExpectingFailure("SELECT * FROM nonexistent").code(), ErrorCode::kNotFound);
  EXPECT_FALSE(engine_.execute("SELECT nosuchcolumn FROM trades").ok());
  EXPECT_FALSE(engine_.execute("SELECT * FROM trades ORDER BY nosuchcolumn").ok());
  EXPECT_FALSE(engine_.execute("SELECT qty / 0 FROM trades").ok());
  // Arithmetic on text is an error; `+` is the one exception, where it
  // concatenates, so multiplication is the honest test of the error path.
  EXPECT_FALSE(engine_.execute("SELECT symbol * 2 FROM trades").ok());
}

TEST_F(SqlFixture, NotBindsLooserThanComparison) {
  // `NOT a = b` must mean `NOT (a = b)`. Parsing NOT as a tight prefix operator
  // -- the C convention -- would silently return the wrong rows instead of
  // failing, which is why this gets its own test.
  seed();
  Result<Statement> parsed = Parser::parse("SELECT * FROM trades WHERE NOT symbol = 'AAPL'");
  ASSERT_TRUE(parsed.ok());
  const auto* select = std::get_if<SelectStatement>(&parsed.value());
  ASSERT_NE(select, nullptr);
  EXPECT_EQ(expressionToString(*select->where), "(NOT (symbol = 'AAPL'))");

  EXPECT_EQ(run("SELECT * FROM trades WHERE NOT symbol = 'AAPL'").rows.size(), 2u);
  EXPECT_EQ(run("SELECT * FROM trades WHERE NOT qty > 60 OR symbol = 'TSLA'").rows.size(), 2u);
}

// ---------------------------------------------------------------------------
// Plan
// ---------------------------------------------------------------------------

TEST_F(SqlFixture, ExposesThePlanTree) {
  seed();
  const ResultSet result = run("SELECT * FROM trades WHERE qty > 60 ORDER BY qty LIMIT 2");
  EXPECT_NE(result.plan.find("Limit"), std::string::npos);
  EXPECT_NE(result.plan.find("Sort"), std::string::npos);
  EXPECT_NE(result.plan.find("Filter"), std::string::npos);
  EXPECT_NE(result.plan.find("SeqScan"), std::string::npos);
}

TEST_F(SqlFixture, LimitStopsPullingRows) {
  run("CREATE TABLE big (n INTEGER)");
  std::string insert = "INSERT INTO big VALUES (0)";
  for (int i = 1; i < 500; ++i) {
    insert += ", (" + std::to_string(i) + ")";
  }
  run(insert);

  // Without ORDER BY there is no pipeline breaker, so the scan must stop after
  // the limit is satisfied -- the Filter's own counter proves it.
  const ResultSet limited = run("SELECT * FROM big WHERE n >= 0 LIMIT 5");
  EXPECT_EQ(limited.rows.size(), 5u);
  EXPECT_NE(limited.plan.find("examined=5"), std::string::npos)
      << "LIMIT did not short-circuit the scan; plan was:\n"
      << limited.plan;
}

TEST_F(SqlFixture, RendersResultsAsJsonAndText) {
  seed();
  const ResultSet result = run("SELECT id, symbol FROM trades WHERE id = 1");
  const std::string json = result.toJson();
  EXPECT_NE(json.find("\"columns\":[\"id\",\"symbol\"]"), std::string::npos);
  EXPECT_NE(json.find("\"AAPL\""), std::string::npos);

  const std::string text = result.toText();
  EXPECT_NE(text.find("symbol"), std::string::npos);
  EXPECT_NE(text.find("AAPL"), std::string::npos);
  EXPECT_NE(text.find("(1 row)"), std::string::npos);
}

TEST_F(SqlFixture, JsonCarriesTheQueryPlan) {
  seed();
  const ResultSet result = run("SELECT id FROM trades WHERE id = 1");
  const std::string json = result.toJson();

  // The plan has to survive serialisation, or EXPLAIN works over RESP and is
  // simply unavailable over HTTP -- and the dashboard, which only speaks HTTP,
  // could never show a query plan.
  ASSERT_NE(json.find("\"plan\":"), std::string::npos) << json;
  EXPECT_NE(json.find("SeqScan"), std::string::npos) << json;
  EXPECT_NE(json.find("Filter"), std::string::npos) << json;

  // Plan text is multi-line and contains quotes and parentheses, so it must be
  // escaped rather than concatenated in raw.
  const ResultSet nothing = run("INSERT INTO trades VALUES (99, 'ZZZZ', 1, 1.0)");
  EXPECT_NE(nothing.toJson().find("\"plan\":\"\""), std::string::npos)
      << "a non-query should report an empty plan, not omit the field";
}

TEST(LikeMatch, HandlesSqlWildcards) {
  EXPECT_TRUE(likeMatch("A%", "AAPL"));
  EXPECT_FALSE(likeMatch("A%", "MSFT"));
  EXPECT_TRUE(likeMatch("%PL", "AAPL"));
  EXPECT_TRUE(likeMatch("A_PL", "AAPL"));
  EXPECT_FALSE(likeMatch("A_PL", "APL"));
  EXPECT_TRUE(likeMatch("%", "anything"));
  EXPECT_TRUE(likeMatch("aapl", "AAPL")) << "LIKE is case-insensitive here";
}
