#include <libpq-fe.h>

#include <fstream>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using Connection = std::unique_ptr<PGconn, decltype(&PQfinish)>;
using Result = std::unique_ptr<PGresult, decltype(&PQclear)>;

struct Inventory {
    int quantity;
    long long offset;
    long long version;
};

struct Event {
    long long offset;
    int delta;
};

void require(bool condition, std::string_view message) {
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

Connection connect(const char* options) {
    Connection connection(PQconnectdb(options), PQfinish);
    require(PQstatus(connection.get()) == CONNECTION_OK, PQerrorMessage(connection.get()));
    return connection;
}

Result query(PGconn* connection, const char* sql, ExecStatusType expected,
             std::initializer_list<std::string> parameters = {}) {
    std::vector<const char*> values;
    for (const auto& parameter : parameters) {
        values.push_back(parameter.c_str());
    }
    Result result(PQexecParams(connection, sql, static_cast<int>(values.size()), nullptr,
                               values.data(), nullptr, nullptr, 0), PQclear);
    require(PQresultStatus(result.get()) == expected, PQerrorMessage(connection));
    return result;
}

Inventory read_inventory(PGconn* connection) {
    auto result = query(connection,
        "SELECT quantity, last_offset, row_version FROM inventory WHERE sku = 'A42'",
        PGRES_TUPLES_OK);
    require(PQntuples(result.get()) == 1, "Expected one inventory row for A42");
    return {std::stoi(PQgetvalue(result.get(), 0, 0)),
            std::stoll(PQgetvalue(result.get(), 0, 1)),
            std::stoll(PQgetvalue(result.get(), 0, 2))};
}

void expect_inventory(PGconn* connection, Inventory expected, std::string_view step) {
    const auto row = read_inventory(connection);
    const auto state = std::to_string(row.quantity) + " / " +
                       std::to_string(row.offset) + " / " + std::to_string(row.version);
    require(row.quantity == expected.quantity && row.offset == expected.offset &&
            row.version == expected.version, std::string(step) + " had unexpected state " + state);
    std::cout << "  " << step << " -> " << state << '\n';
}

void reset(PGconn* connection) {
    query(connection, "TRUNCATE inventory", PGRES_COMMAND_OK);
    query(connection,
          "INSERT INTO inventory (sku, quantity, last_offset, row_version)"
          " VALUES ('A42', 13, 102, 7)", PGRES_COMMAND_OK);
}

std::vector<Event> load_history(const char* path) {
    std::ifstream fixture(path);
    require(fixture.is_open(), "Could not open event fixture");
    std::vector<Event> history;
    Event event{};
    while (fixture >> event.offset) {
        require(static_cast<bool>(fixture >> event.delta), "Invalid event delta in fixture");
        require(history.empty() || history.back().offset < event.offset,
                "Expected increasing offsets in fixture");
        history.push_back(event);
    }
    require(fixture.eof() && !history.empty(), "Invalid event fixture");
    return history;
}

int replay(const std::vector<Event>& history, long long through_offset) {
    int quantity = 0;
    for (const auto& event : history) {
        quantity += event.delta;
        if (event.offset == through_offset) {
            return quantity;
        }
    }
    throw std::runtime_error("Repair offset is missing from fixture");
}

int apply_repair(PGconn* connection, Inventory snapshot, int rebuilt_quantity) {
    auto result = query(connection,
        "UPDATE inventory SET quantity = $1, row_version = row_version + 1"
        " WHERE sku = 'A42' AND row_version = $2 AND last_offset = $3"
        " RETURNING row_version", PGRES_TUPLES_OK,
        {std::to_string(rebuilt_quantity), std::to_string(snapshot.version),
         std::to_string(snapshot.offset)});
    return PQntuples(result.get());
}

int try_sale(PGconn* connection, Inventory snapshot, Event sale) {
    auto result = query(connection,
        "UPDATE inventory SET quantity = $1, last_offset = $2, row_version = row_version + 1"
        " WHERE sku = 'A42' AND row_version = $3 AND last_offset < $2"
        " RETURNING row_version", PGRES_TUPLES_OK,
        {std::to_string(snapshot.quantity + sale.delta), std::to_string(sale.offset),
         std::to_string(snapshot.version)});
    return PQntuples(result.get());
}

bool apply_sale(PGconn* connection, Inventory snapshot, Event sale) {
    while (sale.offset > snapshot.offset) {
        if (try_sale(connection, snapshot, sale) == 1) {
            return true;
        }
        snapshot = read_inventory(connection);
    }
    return false;
}

void sale_first(PGconn* repair, PGconn* consumer, const std::vector<Event>& history, Event sale) {
    std::cout << "Sale commits first\n";
    reset(repair);
    const auto repair_snapshot = read_inventory(repair);
    const auto sale_snapshot = read_inventory(consumer);
    const int proposal = replay(history, repair_snapshot.offset);
    require(proposal == 21, "Expected the initial repair to propose 21");

    require(apply_sale(consumer, sale_snapshot, sale), "Sale did not apply");
    require(apply_repair(repair, repair_snapshot, proposal) == 0, "Stale repair was accepted");
    expect_inventory(repair, {11, 103, 8}, "Stale repair rejected");

    const auto fresh_snapshot = read_inventory(repair);
    require(apply_repair(repair, fresh_snapshot, replay(history, fresh_snapshot.offset)) == 1,
            "Fresh repair did not apply");
    expect_inventory(consumer, {19, 103, 9}, "Fresh repair committed");
}

void repair_first(PGconn* repair, PGconn* consumer, const std::vector<Event>& history, Event sale) {
    std::cout << "Repair commits first\n";
    reset(repair);
    const auto repair_snapshot = read_inventory(repair);
    const auto sale_snapshot = read_inventory(consumer);

    require(apply_repair(repair, repair_snapshot, replay(history, repair_snapshot.offset)) == 1,
            "Repair did not apply");
    expect_inventory(consumer, {21, 102, 8}, "Repair committed");

    require(sale_snapshot.quantity + sale.delta == 11, "Expected a stale sale quantity of 11");
    require(try_sale(consumer, sale_snapshot, sale) == 0, "Stale sale was accepted");
    expect_inventory(consumer, {21, 102, 8}, "Stale sale rejected");

    require(apply_sale(consumer, read_inventory(consumer), sale), "Sale retry did not apply");
    expect_inventory(repair, {19, 103, 9}, "Sale recomputed and committed");

    require(!apply_sale(consumer, sale_snapshot, sale), "Already-applied sale was applied again");
    expect_inventory(repair, {19, 103, 9}, "Already-applied sale skipped after reread");
}

int main(int argc, char** argv) {
    try {
        require(argc == 3, "Expected PostgreSQL connection options and event fixture path");
        const auto history = load_history(argv[2]);
        const auto sale = history.back();
        require(sale.offset == 103 && sale.delta == -2, "Expected a sale of 2 at offset 103");
        auto repair = connect(argv[1]);
        auto consumer = connect(argv[1]);
        require(PQbackendPID(repair.get()) != PQbackendPID(consumer.get()),
                "Expected independent PostgreSQL sessions");

        query(repair.get(),
            "CREATE TABLE inventory (sku text PRIMARY KEY, quantity integer NOT NULL,"
            " last_offset bigint NOT NULL, row_version bigint NOT NULL)", PGRES_COMMAND_OK);
        sale_first(repair.get(), consumer.get(), history, sale);
        repair_first(repair.get(), consumer.get(), history, sale);
    } catch (const std::exception& error) {
        std::cerr << "Demo failed: " << error.what() << '\n';
        return 1;
    }
}
