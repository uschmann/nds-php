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

# date()/time()/mktime() with fixed timestamps, sprintf and str_pad
test-date: basic
	(cat test_date.txt; echo RUN; printf '%s\n' "echo sprintf('%0\$$s', 1);" "echo sprintf('%3\$$s', 1);" \
	 'var_dump(mktime(0, 0, 0, 1, 1, 2040));') \
	 | ./basic > test.out
	diff -u test_date.expected test.out && echo "date test passed"
	rm -f test.out

# exceptions: try/catch/finally, throw, built-in classes, runtime errors as Error
test-exc: basic
	(cat test_exc.txt; echo RUN; echo 'throw new LogicException("uncaught one");'; echo 'echo intdiv(1, 0);'; \
	 echo 'try { echo 1 % 0; } catch (DivisionByZeroError $$e) { echo "mod ", $$e->getMessage(), PHP_EOL; }'; \
	 echo 'throw 5;'; echo 'try { echo 1; }'; echo 'try { throw new Exception("e"); } catch (Nonexistent $$e) { }'; \
	 echo 'echo "still ok", PHP_EOL;'; echo 'echo check(-1);') | ./basic > test.out
	diff -u test_exc.expected test.out && echo "exception test passed"
	rm -f test.out

# debugger: breakpoints, step / over / out, variables (host commands #BP, #DEBUG)
test-dbg: basic
	(cat test_dbg.txt; echo '#BP 2'; echo RUN; echo c; echo c; echo c; echo '#BP 2'; echo '#BP 7'; echo RUN; \
	 echo s; echo s; echo s; echo s; echo s; echo o; echo o; echo q; echo '#BP 7'; echo '#BP 12'; echo RUN; \
	 echo s; echo s; echo o; echo s; echo c; echo '#BP 12'; echo '#DEBUG'; echo RUN; echo o; echo o; \
	 echo u; echo '#BP 14'; echo RUN; echo c; echo 'echo sq(4), PHP_EOL;') | ./basic > test.out
	diff -u test_dbg.expected test.out && echo "debugger test passed"
	rm -f test.out

# match, enums, named arguments
test-types: basic
	(cat test_types.txt; echo RUN; echo 'function v(): void { return 1; }'; echo 'function w(): int { return; }'; \
	 echo 'function d(int $$x = "a") {}'; echo 'function e(void $$x) {}'; echo 'echo add(1, []);'; \
	 echo NEW; cat test_types2.txt; echo RUN) | ./basic > test.out
	diff -u test_types.expected test.out && echo "types test passed"
	rm -f test.out

test-modern: basic
	(cat test_modern.txt; echo RUN; echo 'new Suit();'; echo '$$s->name = "x";'; echo 'echo Dir::from(1);'; \
	 echo 'echo Suit::Clubs;'; echo '$$x = clone Suit::Hearts;'; echo 'echo match(3) { 1, 2 => "a" };'; \
	 echo 'echo card(colour: "x");'; echo 'echo card(rank: 1, rank: 2);'; echo 'echo greet(punct: 1);'; \
	 echo 'echo greet("a", name: "b");'; echo 'echo greet(name: "a", "b");'; echo 'echo strlen(string: "x");'; \
	 echo 'echo match(1) { default => 1, default => 2 };'; echo 'echo Suit::Hearts;') | ./basic > test.out
	diff -u test_modern.expected test.out && echo "modern test passed"
	rm -f test.out

# autocompletion engine: suggestions for several cursor positions
tcomp: test_complete.c ../source/php.c ../source/complete.c ../source/complete.h ../source/basic.h
	$(CC) $(CFLAGS) -Wno-missing-field-initializers -o $@ test_complete.c ../source/php.c ../source/complete.c -lm

test-complete: tcomp
	./tcomp > test.out
	diff -u test_complete.expected test.out && echo "completion test passed"
	rm -f test.out

clean:
	rm -f basic tcomp test.out
