-- Tags: stateful

DROP DICTIONARY IF EXISTS cache_hits;

CREATE DICTIONARY cache_hits
(WatchID UInt64, UserID UInt64, SearchPhrase String)
PRIMARY KEY WatchID
SOURCE(CLICKHOUSE(HOST 'localhost' PORT tcpPort() USER 'default' TABLE 'hits' PASSWORD '' DB 'test'))
LIFETIME(MIN 300 MAX 600)
LAYOUT(CACHE(SIZE_IN_CELLS 100 QUERY_WAIT_TIMEOUT_MILLISECONDS 600000));

SET timeout_before_checking_execution_speed = 300;

SELECT sum(flag) FROM (SELECT dictHas(current_database() || '.cache_hits', toUInt64(WatchID)) as flag FROM test.hits PREWHERE WatchID % 1400 == 0 LIMIT 100);
SELECT count() from test.hits PREWHERE WatchID % 1400 == 0;

SELECT sum(flag) FROM (SELECT dictHas(current_database() || '.cache_hits', toUInt64(WatchID)) as flag FROM test.hits PREWHERE WatchID % 350 == 0 LIMIT 100);
SELECT count() from test.hits PREWHERE WatchID % 350 == 0;

SELECT sum(flag) FROM (SELECT dictHas(current_database() || '.cache_hits', toUInt64(WatchID)) as flag FROM test.hits PREWHERE WatchID % 5 == 0 LIMIT 100);
SELECT count() from test.hits PREWHERE WatchID % 5 == 0;

DROP DICTIONARY IF EXISTS cache_hits;
