# MountX Curated Examples & Projects Suite

A comprehensive collection of **65 functional, standalone examples and real-world projects** written in MountX.

Every single example runs with 100% pass rate on both the **Native C Substrate (`./mountx-native`)** and the **Python Host Reference (`python3 host/main.py`)**.

---

## Running the Examples

To run the entire suite and verify all 65 examples:
```bash
./examples/run_all.sh
```

To run any individual example:
```bash
./mountx-native examples/01_basics/01_hello_world.mx
# OR
python3 host/main.py examples/01_basics/01_hello_world.mx
```

---

## Categories & Contents

### 1. Basics & Language Essentials (`01_basics/`)
- `01_hello_world.mx` - Basic output, string display, and comments.
- `02_variables_types.mx` - Types (`int`, `float`, `string`, `bool`, `null`, `array`) and `type` module inspection.
- `03_control_flow.mx` - `if/else`, `while`, `for-in`, `break`, `continue`.
- `04_functions_recursion.mx` - Functions, parameter handling, and recursion.
- `05_classes_oop.mx` - Classes, fields, methods, constructors (`init`), `this`.
- `06_inheritance_super.mx` - Inheritance, method overriding, and `super()` calls.
- `07_try_catch_throw.mx` - Exception handling with `tryx`, `catchx`, `throwx`, and `error.attempt`.
- `08_tuples.mx` - Immutable sequence data structures using `tuple.mx`.
- `09_enums.mx` - Type-safe constants and state enumerations using `enum.mx`.
- `10_lambda_iter.mx` - Sequence generation (`range`, `enumerate`, `zip`, `windows`, `chunks`) via `iter.mx`.

### 2. Data Structures (`02_data_structures/`)
- `11_arrays.mx` - High-level array operations (`first`, `last`, `reversed`, `sort_by`, `unique`, `filter`, `map`).
- `12_maps_dicts.mx` - Key-value stores using `dict.Dict` and pair maps.
- `13_sets.mx` - Unique collections, union, intersection, and difference via `set.Set`.
- `14_hash_tables.mx` - Separate chaining hash table with dynamic buckets via `hashtable.HashTable`.
- `15_stack.mx` - LIFO Stack with balanced bracket matching algorithm.
- `16_queue.mx` - FIFO Queue simulating asynchronous task processing.
- `17_linked_list.mx` - Doubly linked list with `add_front`, `add_back`, `remove_first`, `reverse`.
- `18_binary_search_tree.mx` - Binary Search Tree (BST) with sorted in-order traversal via `bst.BST`.
- `19_graph.mx` - Graph modeling, adjacency lists, BFS, DFS, and shortest path.
- `20_collections_counter.mx` - Frequency counter and priority grouping.

### 3. Math, Statistics & Algorithms (`03_math_and_algorithms/`)
- `21_basic_math.mx` - `abs`, `min`, `max`, `pow`, `factorial`, `clamp` via `math.mx`.
- `22_math_advanced.mx` - `idiv`, `floor`, `ceil`, `round`, `gcd`, `lcm`, `sqrt`, `is_prime`, `to_base`.
- `23_random_numbers.mx` - Pseudo-random integers, chance, choice, shuffle, and seeded generator via `random.mx`.
- `24_statistics.mx` - Mean, median, variance, and standard deviation calculations.
- `25_quicksort.mx` - Pure functional QuickSort sorting algorithm.
- `26_binary_search.mx` - Logarithmic $O(\log n)$ binary search on sorted collections.
- `27_fibonacci_memo.mx` - Dynamic programming with memoization vs iterative Fibonacci.
- `28_prime_sieve.mx` - Sieve of Eratosthenes generating prime numbers up to $N$.
- `29_dijkstra_shortest_path.mx` - Dijkstra's shortest path routing on weighted graphs.
- `30_matrix_operations.mx` - 2D matrix multiplication and transposition.

### 4. Text, Regex & Serialization (`04_text_and_regex/`)
- `31_string_methods.mx` - `trim`, `to_upper`, `to_lower`, `split`, `join`, `replace`, `contains`.
- `32_regex_patterns.mx` - Regular expression matching with quantifiers and character classes via `regex.mx`.
- `33_json_parsing.mx` - Decoding JSON strings into structured pairs and lists via `json.parse`.
- `34_json_serializing.mx` - Encoding MountX data structures into JSON strings via `json.stringify`.
- `35_csv_parser.mx` - Comma-Separated Values (CSV) parser with column headers.
- `36_text_templating.mx` - Lightweight template engine substituting `{{placeholders}}`.
- `37_word_frequency.mx` - Text analyzer counting unique words and frequencies.
- `38_caesar_cipher.mx` - Symmetric encryption and decryption using `str_ord` and `str_chr`.

### 5. System, Files & OS (`05_system_and_io/`)
- `39_file_io.mx` - Safe file reading, writing, and appending with `io.mx`.
- `40_os_filesystem.mx` - Cross-platform path operations (`path_join`, `path_basename`, `path_ext`, `cwd`).
- `41_directory_traversal.mx` - Listing directory contents and filtering extensions with `os.listdir_ext`.
- `42_env_variables.mx` - Reading environment variables with fallback defaults (`os.env_or`).
- `43_system_exec.mx` - Executing shell commands and capturing stdout / exit codes with `os.system` / `sys_exec`.
- `44_process_runner.mx` - Subprocess runner with `ProcessResult` inspection via `process.mx`.
- `45_date_time.mx` - Current timestamps and date formatting via `datetime.mx`.
- `46_benchmarking.mx` - High-precision millisecond timing with `time.now_ms`.
- `47_cli_arguments.mx` - Command-line flag and option parsing with `args.ArgParser`.
- `48_buffer_binary.mx` - Raw byte buffer manipulation and hex conversion via `buffer.mx`.

### 6. Database & SQLite (`06_database_sqlite/`)
- `49_sqlite_basics.mx` - In-memory SQLite database creation, table DDL, and queries.
- `50_sqlite_crud.mx` - Parameterized CRUD queries (`INSERT`, `SELECT`, `UPDATE`, `DELETE`).
- `51_sqlite_schema_inspection.mx` - Querying `sqlite_master` and inspecting column PRAGMA metadata.
- `52_sqlite_relational.mx` - Relational `INNER JOIN` queries across multiple tables.
- `53_sqlite_aggregations.mx` - `COUNT`, `SUM`, `AVG`, and `GROUP BY` analytics queries.
- `54_sqlite_persistent_db.mx` - Disk-backed SQLite database persistence test.

### 7. Networking & Web (`07_networking_and_web/`)
- `55_url_parsing.mx` - URL dissection into scheme, host, port, path, query, fragment via `url.parse_url`.
- `56_query_encoding.mx` - URL encoding/decoding and query string generation (`net.query_encode`).
- `57_http_request_builder.mx` - HTTP/1.1 message formatting and response parser.
- `58_http_get_fetch.mx` - Live HTTPS GET request using native `net.fetch(url)`.
- `59_http_post_json.mx` - Live HTTPS POST request with JSON payload using native `net.post(url, body, type)`.
- `60_api_client.mx` - Object-oriented REST API Client class wrapper.

### 8. Standalone Real-World Projects (`08_projects/`)
- **Project 1: SQLite To-Do List CLI (`01_todo_cli/main.mx`)**
  - Interactive task management application with priorities, completion flags, and SQLite persistence.
- **Project 2: Static Site Documentation Generator (`02_static_site_generator/main.mx`)**
  - Generates responsive HTML documentation with navigation headers, custom CSS styling, and timestamps.
- **Project 3: Unit Testing & Assertion Framework (`03_test_framework/main.mx`)**
  - Complete test runner class with `assert_eq`, `assert_true`, execution metrics, and reporting.
- **Project 4: Personal Expense Tracker (`04_expense_tracker/main.mx`)**
  - Financial budget calculator with category breakdowns, transaction logs, and summary reports.
- **Project 5: Public Weather REST API Client (`05_weather_api_client/main.mx`)**
  - Live HTTPS query client parsing JSON and formatting terminal card layouts.
