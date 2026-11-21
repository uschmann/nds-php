/* test_refactor.c - rename / definition / usages on a small project */
#include <stdio.h>
#include <string.h>
#include "../source/basic.h"
#include "../source/refactor.h"

static void h_putch(char c) { (void)c; }

static const char *kinds[] = { "none", "var", "member", "class", "name" };

static void dump(void) {
	for (int f = 0; f < basic_file_count(); f++) {
		printf("--- %s\n", basic_file_name(f));
		for (int i = 0; i < basic_file_line_count(f); i++) printf("%2d %s\n", i + 1, basic_file_line_text(f, i));
	}
}

/* find text in file f and look at the word there */
static RefTarget at(int f, const char *needle, int off) {
	RefTarget t = { 0 };
	for (int i = 0; i < basic_file_line_count(f); i++) {
		const char *p = strstr(basic_file_line_text(f, i), needle);
		if (p) { ref_target(f, i, (int)(p - basic_file_line_text(f, i)) + off, &t); break; }
	}
	return t;
}

static void show(const char *label, RefTarget t) {
	RefOcc o[REF_MAX];
	int n = ref_find(&t, o, REF_MAX), df, dl, dc;
	printf("%-22s %s %s", label, kinds[t.kind], t.name);
	if (ref_definition(&t, &df, &dl, &dc)) printf(" def %d:%d:%d", df, dl + 1, dc);
	printf(" |");
	for (int i = 0; i < n; i++) printf(" %d:%d:%d%s%s", o[i].file, o[i].line + 1, o[i].col, o[i].code ? "" : "(txt)", o[i].decl ? "*" : "");
	printf("\n");
}

int main(void) {
	static const BasicIO io = { h_putch, NULL, NULL, NULL, NULL, 32, NULL };
	basic_init(&io);
	const char *main_php[] = {
		"require 'lib';",
		"const LIMIT = 3;",
		"$total = 0;",
		"function add($a, $b) {",
		"  $total = $a + $b; // local $total",
		"  return $total;",
		"}",
		"$p = new Player('Ann', 5);",
		"$p->addPoints(LIMIT);",
		"$total = add($p->score, 1);",
		"echo \"Total: $total {$p->name}\";",
		"echo 'add $total';",
		"$f = fn($x) => $x + $total;",
		"/* add",
		"   $total */ echo Suit::Hearts->value, Player::$count;",
		"echo strlen('x');",
		NULL
	};
	for (int i = 0; main_php[i]; i++) basic_insert_line(basic_line_count(), main_php[i]);
	int ln;
	basic_enter("EDIT \"lib\"", &ln);
	while (basic_state() == BASIC_RUNNING) basic_run_slice(100);
	const char *lib_php[] = {
		"enum Suit: string {",
		"  case Hearts = 'H';",
		"  public function label(): string { return self::Hearts->name; }",
		"}",
		"class Player {",
		"  public static int $count = 0;",
		"  public function __construct(public string $name, public int $score = 0) {",
		"    self::$count++;",
		"  }",
		"  public function addPoints(int $n): void { $this->score += $n; }",
		"  public function best(Player $other): Player { return $this; }",
		"}",
		NULL
	};
	for (int i = 0; lib_php[i]; i++) basic_insert_line(basic_line_count(), lib_php[i]);
	basic_select_file(0);

	show("global $total", at(0, "$total = 0", 1));
	show("local $total", at(0, "  $total = $a", 3));
	show("param $a", at(0, "add($a", 5));
	show("fn $x", at(0, "fn($x", 4));
	show("function add", at(0, "add($p", 0));
	show("const LIMIT", at(0, "LIMIT);", 2));
	show("class Player", at(0, "new Player", 6));
	show("method addPoints", at(0, "addPoints(LIMIT", 3));
	show("prop score", at(1, "->score", 3));
	show("promoted name", at(1, "string $name", 9));
	show("static count", at(0, "$count", 2));
	show("enum case", at(0, "Hearts->", 0));
	show("enum Suit", at(1, "enum Suit", 6));
	show("builtin strlen", at(0, "strlen", 1));
	show("keyword echo", at(0, "echo strlen", 1));
	show("string word", at(0, "'add", 2));
	show("$this", at(1, "$this->score", 2));
	int s, e;
	const char *w = "  $total = add($a);";
	printf("word %d", ref_word(w, 3, &s, &e)); printf(" %d-%d", s, e);
	printf(" | %d", ref_word(w, 8, &s, &e)); printf(" %d-%d", s, e);
	printf(" | %d", ref_word(w, 9, &s, &e)); printf(" | %d", ref_word(w, 2, &s, &e)); printf(" %d-%d\n", s, e);

	char err[64];
	RefTarget t = at(0, "$total = 0", 1);
	printf("rename global $total -> sum: %d\n", ref_rename(&t, "sum", err, sizeof err));
	t = at(1, "class Player", 7);
	printf("rename Player -> Gamer: %d\n", ref_rename(&t, "Gamer", err, sizeof err));
	t = at(1, "->score", 3);
	printf("rename score -> points: %d\n", ref_rename(&t, "points", err, sizeof err));
	t = at(0, "add($p", 0);
	printf("rename add -> 1x: %d %s\n", ref_rename(&t, "1x", err, sizeof err), err);
	printf("rename add -> echo: %d %s\n", ref_rename(&t, "echo", err, sizeof err), err);
	printf("rename add -> plus: %d\n", ref_rename(&t, "plus", err, sizeof err));
	printf("current file %d\n", basic_current_file());
	dump();
	return 0;
}
