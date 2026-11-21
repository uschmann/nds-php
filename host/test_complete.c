/* test_complete.c - prints the autocompletion for some cursor positions */
#include <stdio.h>
#include <string.h>
#include "../source/basic.h"
#include "../source/complete.h"

static void h_putch(char c) { putchar(c); }

static void show(const char *before) {
	Completion c[COMPLETE_MAX];
	int wl, n = complete_find(before, c, COMPLETE_MAX, &wl);
	printf("[%s] word=%d:", before, wl);
	for (int i = 0; i < n && i < 10; i++) printf(" %s", c[i].label);
	if (n) printf("  | %s -> \"%s\" back %d", c[0].hint, c[0].insert, c[0].back);
	printf("\n");
}

int main(void) {
	static const BasicIO io = { h_putch, NULL, NULL, NULL, NULL, 32, NULL };
	static const char *help[] = { "str_pad($s,n,c)`auffüllen", "strlen($s)`Länge", "count($a)`Anzahl" };
	basic_init(&io);
	complete_set_help(help, 3);
	const char *prog[] = {
		"enum Suit: string {", "  case Hearts = 'H';", "  case Spades = 'S';", "  const WILD = 'j';",
		"  public function color(): string { return 'red'; }", "}",
		"class Player {", "  public $name;", "  public function __construct(public int $score = 0) {}",
		"  public function addPoints($n, $bonus = 0) { $this->score += $n; }", "}",
		"function strangeHelper($a, $b) { return $a; }", "$players = [];", "$playerCount = 0;",
		"// $commented = 1;", "echo \"$inString\";", NULL
	};
	for (int i = 0; prog[i]; i++) basic_insert_line(basic_line_count(), prog[i]);
	show("st");
	show("echo str_p");
	show("$pl");
	show("$p->");
	show("$p->add");
	show("Suit::");
	show("Suit::H");
	show("$x = new ");
	show("$x = new Inv");
	show("} catch (Run");
	show("fe");
	show("tc");
	show("fore");
	show("JOY_");
	show("echo \"st");
	show("// st");
	show("1");
	show("s");
	show("count");
	complete_used("strtoupper");
	show("st");
	return 0;
}
