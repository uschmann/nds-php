CFLAGS := -Wall -Wextra -O2 -g

basic: host_main.c ../source/php.c ../source/basic.h
	$(CC) $(CFLAGS) -o $@ host_main.c ../source/php.c -lm

# language test: compares the output with test_php.expected
test: basic
	(cat test_php.txt; echo RUN; echo Alice; echo 'echo 6*7, PHP_EOL;'; echo 'echo fib(12), PHP_EOL;'; \
	 echo 'echo 1 +;'; echo 'foo();'; echo NEW; echo '10 $$x = 0;'; echo '20 echo 1 / $$x;'; echo RUN) | ./basic > test.out
	diff -u test_php.expected test.out && echo "language test passed"
	rm -f test.out

# classes, interfaces, traits: compares the output with test_oop.expected
test-oop: basic
	(cat test_oop.txt; echo RUN; echo 'echo $$shapes[0]->r;'; echo 'echo Counter::$$n;'; echo 'new Base();'; \
	 echo '$$p->nothing();'; echo 'echo Circle::NOPE;'; echo 'class X {}'; echo 'echo count($$shapes), PHP_EOL;') | ./basic > test.out
	diff -u test_oop.expected test.out && echo "oop test passed"
	rm -f test.out

test-files: basic
	rm -rf files.tmp && mkdir files.tmp
	printf '%s\n' '10 echo "HI";' '20   $$x = 1; // indented' 'SAVE "hello"' 'SAVE "two.php"' 'DIR' 'NEW' 'LIST' \
		'LOAD "HELLO"' 'LIST' 'SAVE' 'DELETE "TWO"' 'DIR' 'LOAD "NOPE"' 'SAVE "BAD NAME"' \
		'SAVE "TOOLONGNAME"' | BASIC_DIR=files.tmp ./basic
	cat files.tmp/HELLO/main.php
	@echo "--- without storage:"; echo 'SAVE "X"' | ./basic
	rm -rf files.tmp

test-examples: basic
	rm -rf files.tmp && mkdir files.tmp && cp -r ../examples/PONG ../examples/SNAKE files.tmp/
	(echo 'LOAD "SNAKE"'; echo RUN) | BASIC_DIR=files.tmp timeout 20 ./basic | tail -3
	(echo 'LOAD "PONG"'; echo FILES; echo RUN) | BASIC_DIR=files.tmp timeout 60 ./basic | tail -3
	(echo 'LOAD "PONG"'; echo 'SAVE "COPY"') | BASIC_DIR=files.tmp ./basic
	diff -r ../examples/PONG files.tmp/COPY && echo "save/load round trip identical"
	rm -rf files.tmp

# several files: require, errors with file name, EDIT/RENAME/RMFILE, project folders
test-multi: basic
	rm -rf files.tmp && mkdir files.tmp
	printf '%s\n' '10 require "util.php";' '20 echo greet("DS"), PHP_EOL;' 'EDIT "util.php"' \
		'10 function greet($$n) { return "Hi $$n"; }' '20 function oops() { return 1 / 0; }' \
		'FILES' 'EDIT main' 'RUN' 'SAVE "MULTI"' 'NEW' 'LOAD "MULTI"' 'FILES' 'RUN' \
		'30 oops();' 'RUN' 'RENAME "util.php" "lib"' 'RUN' 'RMFILE "lib"' 'SAVE' 'DIR' \
		| BASIC_DIR=files.tmp ./basic
	ls files.tmp/MULTI
	rm -rf files.tmp

# files and JSON: data files live in the program folder (or the storage root)
test-io: basic
	rm -rf files.tmp && mkdir files.tmp
	(cat test_io.txt; echo RUN; echo 'SAVE "GAME"'; echo 'echo file_get_contents("score.json");'; echo RUN; \
	 echo 'file_put_contents("x.php", "");'; echo 'file_get_contents("../x");'; echo 'sfx("boom");'; \
	 echo 'tone("H4");'; echo 'echo json_encode(json_decode("[1,{\"a\":[]}]"));') \
	 | BASIC_DIR=files.tmp ./basic > test.out
	ls files.tmp files.tmp/GAME >> test.out
	diff -u test_io.expected test.out && echo "io test passed"
	rm -rf files.tmp test.out

# closures, arrow functions and callbacks (array_map, usort, ...); REPL closures stay callable
test-closure: basic
	(cat test_closure.txt; echo RUN; echo 'print_r($$double);'; echo 'var_dump(fn() => 1);'; echo 'echo json_encode([$$inc]);'; \
	 echo 'echo $$double;'; echo '$$x = 5;'; echo '$$nf = "nofunc"; $$nf();'; echo 'echo count($$people), PHP_EOL;'; \
	 echo 'echo array_map(fn($$x) => $$x + $$q, [1])[0], PHP_EOL;'; echo '$$q = 7; echo (fn($$x) => $$x + $$q)(1), PHP_EOL;'; \
	 echo 'echo array_map(fn($$x) => fn($$y) => $$x * $$y, [2, 3])[1](4), PHP_EOL;'; \
	 echo '$$f = function() use ($$x, $$x) {};'; echo '$$g = function($$a) use ($$a) {};'; \
	 echo '$$t = 0; $$w = function() use (&$$t) {};'; \
	 echo 'array_walk($$m, function($$v, $$k) { echo "$$k=$$v "; }); echo PHP_EOL;'; \
	 echo '$$h = fn($$x) => $$x * 3;'; echo 'echo 1 + 2, PHP_EOL;'; echo 'echo $$h(2), " ", strcmp("a", "b"), PHP_EOL;'; \
	 echo 'usort($$people, fn($$a, $$b) => $$a["n"] <=> $$b["x"] + 1 / 0);'; echo 'echo $$people[0]["n"], PHP_EOL;') \
	 | ./basic > test.out
	diff -u test_closure.expected test.out && echo "closure test passed"
	rm -f test.out

clean:
	rm -f basic test.out
