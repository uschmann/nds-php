# NDS PHP

*English · [Deutsch](README.md)*

A small, retro-inspired PHP interpreter for the Nintendo DS – you program right on the device like on
an old home computer, but in a PHP dialect.

* **Top screen:** code editor with line numbers and status line, or the REPL console with the
  program output
* **Bottom screen:** touch keyboard with a status card; the interface is light, with red as the accent colour
* **Title screen:** on start-up the words "NDS PHP" rain down as 3D voxels on the top screen and build
  the logo, rendered with the DS 3D hardware. The bottom screen lists all saved programs
  (12 per page, ← → to flip pages): tapping one loads it straight into the editor. "NEW PROGRAM"
  or "BACK TO PROGRAM", or any button, starts the editor without loading anything.
  `EXIT` (in the REPL) returns to the title screen; program and data are kept

## Building

Only Docker is required (image `devkitpro/devkitarm`):

```bash
./build.sh          # creates nds-php.nds
./build.sh clean
```

Interpreter tests on the PC (no DS needed):

```bash
make -C host test test-files test-examples
```

## Controls

The top screen is either the **code editor** or the **REPL console**; `SCRN` switches between them.

| Action | Keyboard | Buttons |
|---|---|---|
| Run / stop the program | `RUN` / `STOP` | START |
| Switch between editor and REPL console | `SCRN` | – |
| Help page with all commands (↑↓ scroll, ←→ page) | – | SELECT |
| Move the cursor | `←` `→` `↑` `↓` | d-pad |
| Line start / end, page up / down | – | R + ←→, R + ↑↓ (in the console also L) |
| New line (keeps the indentation) | `RET` | A |
| Delete left of the cursor (at the line start: join lines) | `DEL` | B |
| Delete / cut the whole line | `CLR` | X (in the editor) |
| Duplicate the line (copy below, cursor on the copy) | – | Y (in the editor) |
| Capitals and second symbols (tap twice = lock) | `SHFT` | hold R (in the console also L) |
| Code on the touch screen (select, rename …) | – | hold / tap L (in the editor) |
| Paste / undo / redo | – | SELECT + X / Y / A (in the editor) |
| Menu `⋮` (right of the tab bar): open (file browser), save, debugger, sprite editor, sound editor, exit (to the title screen) | – | – |
| Sprite editor | – | X (in the REPL console) |
| Sound editor | – | Y (in the REPL console) |

The keyboard types lower case; shift gives capitals and the characters PHP needs,
`\ { } [ ] | : *` (shown at the top left of each key).

**Editor:** the code is edited in place – put the cursor where you want and type. The line numbers
in the left margin are only there for orientation (and error messages); you never type them. Long
lines wrap. After a line ending in `{`, `RET` indents two more characters, and a `}` on an otherwise
empty line indents back. When a program stops with an error, the cursor sits on the error line when
you switch back.

**Code on the touch screen (refactoring):** In the editor, **hold L** to swap the screens: the code
moves to the touch screen, the keyboard to the top. **Tap L** to keep them swapped until you tap L
again (or *Done*). With the stylus:

| Gesture | Effect |
|---|---|
| tap a word | select the word (`$x` including the `$`) |
| tap next to it | just place the cursor |
| drag up / down | select whole lines (scrolls at the top / bottom edge) |
| drag sideways in a line | select text in the line |
| swipe over the line numbers | scroll |

Below the code is a toolbar that depends on the selection:

* **word:** *Rename*, *Def* (jump to the definition), *Uses* (all usages as a list; tap one to
  jump there), *Copy*, *Cut*
* **lines:** *Cut*, *Copy*, *Dup*, `«` `»` outdent / indent, `↑` `↓` move the block
* **text in a line:** *Cut*, *Copy*
* **nothing:** *Paste*, *Undo*, *Redo*

**Rename** shows every place in all files on the top screen with the new name in green; matches in
strings and comments are grey and stay as they are. Type the new name, `RET` renames, `SCRN`
cancels. What belongs together follows from the place:

| Word | renamed |
|---|---|
| `$x` in a function | only in that function (closures and `fn` inside belong to it) |
| `$x` outside functions | in the global code of all files, also in `"…$x…"` |
| function / constant | declaration and all calls |
| class, interface, trait, enum | declaration, `new`, `extends`, types, `X::`, `catch` … |
| method, property, constant, enum case | declaration and every `->name` / `::name` |

Methods and properties are recognized by name only: if two classes use the name, both are renamed.
Built-in functions and classes cannot be renamed.

Selected lines stay selected after swapping back: **X** cuts them, **Y** duplicates them. Without a
selection, X cuts the current line (into the clipboard). **SELECT+X** pastes, **SELECT+Y** undoes,
**SELECT+A** redoes (the last 20 changes; a rename across several files counts as one). In the
editor only **R** is shift now.

**Autocompletion:** While typing in the editor and at the REPL, tappable suggestions appear above
the keyboard, with the signature of the first one below (taken from the help page for built-in
functions, with their parameters for your own). Tapping takes a suggestion; functions get their
`(` right away. Pressing and releasing the R shoulder button on its own takes the first suggestion,
the arrow on the right shows more. What is suggested depends on the text before the cursor:

| Before the cursor | Suggestions |
|---|---|
| a word of 2+ letters | own and built-in functions, classes, keywords, constants, snippets |
| `$…` | the program's variables |
| `->…` | methods and properties of the program's classes |
| `Class::…` | constants, enum cases and methods of that class, `cases`/`from`/`tryFrom` for enums |
| `new `, `instanceof `, `catch (`, `extends ` | your classes and the built-in exceptions |

Snippets insert whole constructs and put the cursor in the right place: `fe` →
`foreach ($ as $v) {`, `fo` → `for`, `wh` → `while`, `if`, `sw` → `switch`, `ma` → `match`, `fn` →
`function () {`, `tc` → `try { } catch (Exception $e) { }`, `cl` → `class  {`, `co` →
`public function __construct(public $) {}` (property promotion), `pf` → `public function () {`. Recently taken suggestions come first,
then your own names before built-in ones. There are no suggestions inside strings and comments.

**REPL console:** it shows the program output and has a `>` prompt where you run single PHP
statements right away (`echo 2 + 3;`, `$x = strlen("abc");`) or type commands such as `RUN`,
`SAVE "NAME"` and `LOAD "NAME"`. An expression without `echo` shows its result right away
(`1 + 2` → `= 3`, `strtoupper("ds")` → `= "DS"`, arrays briefly as `[1, 2]`, long arrays and objects with one element per line, all coloured like
in the editor); assignments, `null` and
functions that print themselves (`printf`, `print_r` …) stay quiet. The input is highlighted like code in the editor, commands such as `RUN` in red. ↑↓
bring back earlier input. `START` or `RUN` start the program from
the editor and switch to the console for it.

**Language:** the user interface (title screen, hints, help page) follows the system language of the DS.
If the DS is set to German it appears in German, with any other language in English.
Error messages and commands are always English.

## The PHP dialect

`RUN` joins all lines in order into one program, compiles it to bytecode and runs it. So `{ }` blocks
may span any number of lines; `<?php` at the start is allowed but not needed. Errors are reported with
the line number (`Parse error in line 3: unexpected 'echo'`, `Error in line 2: Division by zero`).

```php
function fib($n) {
  return $n < 2 ? $n : fib($n - 1) + fib($n - 2);
}
for ($i = 0; $i < 10; $i++) echo fib($i), " ";
$name = input("NAME? ");
echo "\nHELLO $name!";
```

Supported:

* **Values:** `int` (32 bit), `float`, strings, `true`/`false`/`null`, arrays as ordered maps with
  integer or string keys (`[1, 2]`, `['a' => 1]`, `array(…)`, `$a[] = 3`); arrays are copied by value
  as in PHP
* **Strings:** `'…'` and `"…"` with `\n \t \\ \" \$` and variables (`"Hello $name"`, `"$a[0]"`,
  `"{$a['x']['y']}"`)
* **Operators:** `+ - * / % **`, `.`, `== != === !== < > <= >= <=>`, `&& || ! and or xor`,
  `& | ^ ~ << >>`, `?:`, `??`, `= += -= *= /= .= %= **= &= |=`, `++ --`, casts `(int) (float) (string) (bool) (array)`
* **Statements:** `echo`, `print`, `if/elseif/else`, `while`, `do…while`, `for`, `foreach ($a as $k => $v)`,
  `switch/case/default`, `break n`, `continue n`, `function name($a, $b = 1) { … return …; }`,
  `global`, `unset`, `isset`, `empty`, `exit`/`die`, comments `//`, `#`, `/* */`
* **Functions:** `strlen substr strpos str_repeat str_pad str_replace strcmp strcasecmp str_contains str_starts_with
  str_ends_with strtoupper strtolower ucfirst lcfirst trim ltrim rtrim strrev explode implode sprintf
  printf vsprintf vprintf number_format chr ord intval floatval strval boolval is_numeric is_int is_float is_string
  is_bool is_array is_null gettype get_debug_type abs floor ceil round sqrt pow sin cos tan asin acos atan atan2
  exp log log10 pi fmod intdiv deg2rad rad2deg min max rand mt_rand srand count in_array
  array_search array_key_exists array_keys array_values array_sum array_reverse array_slice
  array_merge array_fill range array_push array_pop array_shift array_unshift sort rsort shuffle
  print_r var_dump time date mktime checkdate`, with callbacks: `array_map array_filter array_reduce array_walk usort uasort uksort
  call_user_func call_user_func_array is_callable function_exists`
* **Constants:** `PHP_EOL`, `PHP_INT_MAX`, `M_PI`, `STR_PAD_LEFT/RIGHT/BOTH`, `JOY_UP` … `JOY_R`,
  your own with `const NAME = value;`
* **Closures and arrow functions** (`function () use ($x) { … }`, `fn ($x) => …`) – see below
* **Classes, interfaces and traits** – see below
* **Exceptions** (`try`/`catch`/`finally`, `throw`) – see below
* **PHP 8:** `match`, `enum`, named arguments, type declarations and `readonly` – see below

Not included are, among others, references (`&`), static variables in functions and
namespaces (a leading `\` as in `\Exception` is allowed). As in PHP, keywords and function names are case-insensitive, variables are not.
Numbers with a decimal point are single-precision `float` on the DS.

In the **REPL** the variables and functions of the last `RUN` are kept
(`echo $score;`, `echo fib(12);`). `CLR` clears all variables.

### Classes, interfaces and traits

```php
interface Shape {
  public function area(): float;
}

trait Describes {
  public function describe() { return $this->name() . ": " . round($this->area(), 1); }
}

abstract class Base implements Shape {
  use Describes;
  protected static $count = 0;
  public function __construct(protected string $color = "red") { static::$count++; }
  public function name(): string { return $this->color . " " . static::KIND; }
  public function __toString(): string { return "[" . $this->name() . "]"; }
}

class Circle extends Base {
  const KIND = "circle";
  public function __construct(private float $r) { parent::__construct("blue"); }
  public function area(): float { return M_PI * $this->r ** 2; }
}

$c = new Circle(2);
echo $c->describe(), " ", $c, PHP_EOL;     // blue circle: 12.6 [blue circle]
var_dump($c instanceof Shape);              // bool(true)
```

* **Classes:** `class`, `extends`, `implements`, `abstract`, `final`, properties with defaults
  (arrays too), methods, `const`, `static` properties and methods, visibility
  `public`/`protected`/`private` (enforced), constructor `__construct()` with promotion
  (`public function __construct(private int $x)`); type declarations are checked – see “Types”
* **Objects:** `new A(…)`, `new static`/`new self`, `$this`, `$o->x`, `$o->f()`, chains such as
  `$this->items[] = $x` or `(new A)->f()->g()`, `$o->$name`, `A::X`, `A::$n`, `A::f()`, `self::`,
  `parent::`, `static::` (late static binding), `A::class`, `instanceof`, `clone`, `__toString()`
  for `echo`, concatenation and strings, `foreach` over the visible properties, `print_r`/`var_dump`.
  As in PHP, objects are shared, not copied
* **Interfaces:** methods without body and constants, `extends` several interfaces; `RUN` checks
  that concrete classes implement all methods
* **Traits:** properties and methods, mixed into classes with `use A, B;`; every class gets its own
  static properties. There is no conflict resolution (`insteadof`, `as`)
* **Functions:** `get_class`, `get_parent_class`, `get_object_vars`, `method_exists`,
  `property_exists`, `class_exists`, `interface_exists`, `is_object`

Classes are declared in the program (not in the REPL), but may live in any file and come before or
after their use. There are no magic methods other than `__construct` and `__toString`.

### match, enum and named arguments

`match` compares strictly (`===`), returns a value and allows several conditions per arm. If
nothing matches and there is no `default`, it throws an `UnhandledMatchError`:

```php
echo match ($grade) { 1, 2 => "good", 3, 4 => "ok", default => "meh" };
$text = match (true) { $temp < 0 => "frost", $temp < 20 => "fresh", default => "warm" };
```

Enums come pure or backed (`int` or `string`), with methods, constants, static methods and
interfaces. Every case is an object that exists only once (so `===` compares cases):

```php
enum Colour: string {
  case Red = 'r';
  case Green = 'g';
  public function light(): bool { return $this === self::Green; }
}
$c = Colour::from('r');                 // Colour::Red, tryFrom() returns null instead of an error
echo $c->name, " ", $c->value;          // Red r
foreach (Colour::cases() as $x) echo $x->name;
echo match ($c) { Colour::Red => "stop", Colour::Green => "go" };
```

`name` and `value` are read-only; enums cannot be created with `new`. `json_encode()` writes the
value of backed enums. All enums implement `UnitEnum`, backed ones also `BackedEnum`.

**Named arguments** pass values by parameter name, also after ordinary arguments; optional
parameters left out keep their default. This works for your own functions, methods, constructors
and closures (not for built-in functions):

```php
function card(string $suit = 'H', int $rank = 1, bool $face = false) { ... }
card(rank: 12, face: true);
$p = new Point(y: 5);
```

### Types

Type declarations are checked at run time like in PHP 8: on parameters, return values and
properties. Supported are `int`, `float`, `string`, `bool`, `array`, `callable`, `iterable`,
`object`, `mixed`, `null`, `false`, `true`, class and interface names, `self`/`static`/`parent`,
nullable (`?int`) and union types (`int|string|null`), and for return values also `void` and `never`.

```php
function plus(int $a, int $b): int { return $a + $b; }
echo plus(2, "3");                     // 5: "3" becomes 3
plus("abc", 1);                        // TypeError: plus(): Argument #1 ($a) must be of type int, string given

class Point {
  public function __construct(public int $x = 0, public readonly ?string $name = null) {}
}
$p = new Point(x: "5");                 // $p->x is int(5)
$p->x = "ten";                          // TypeError: Cannot assign string to property Point::$x of type int
$p->name = "new";                       // Error: Cannot modify readonly property Point::$name
```

* **Conversion:** By default PHP converts fitting scalars (`"5"` → `5`, `5` → `"5"`, `1` → `true`).
  With `declare(strict_types=1);` the type must match exactly; only `int` still widens to `float`.
  Here the declaration applies to the whole program, not just the one file.
* **Properties:** `public int $n = 0;` checks every assignment. A typed property without a default
  starts uninitialized: reading it is an error, while `isset()` and `??` give `false` or the
  fallback.
* **`readonly`:** the property is set once from inside the class (usually in the constructor) and
  can only be read after that. `readonly class` makes every property readonly. Constructor
  promotion carries over the type and `readonly`.
* **Errors:** Wrong types throw a `TypeError` that you can `catch`. Mistakes the compiler can see
  already are parse errors, e.g. `return 1;` in a `void` function or a default of the wrong type.
* **Helpers:** `gettype()` and `get_debug_type()` (`int`, `float`, `Point`, `Closure` …).
* **Not included:** intersection types (`A&B`), type checks for built-in functions and static
  properties, and signature checks when overriding methods.

### Exceptions

`try`, `catch` and `finally` work as in PHP 8, including several classes (`catch (A | B $e)`),
catching without a variable (`catch (Exception)`) and `throw` as an expression
(`$x ?? throw new Exception("empty")`).

```php
function divide($a, $b) {
  if ($b == 0) throw new InvalidArgumentException("Division by zero", 1);
  return $a / $b;
}
try {
  echo divide(10, 0);
} catch (InvalidArgumentException $e) {
  echo "Error in line ", $e->getLine(), ": ", $e->getMessage();
} finally {
  echo "\ndone";
}
```

Built in are `Throwable`, `Exception` and `Error` with the usual subclasses (`RuntimeException`,
`LogicException`, `InvalidArgumentException`, `OutOfRangeException`, `UnexpectedValueException`,
`TypeError`, `DivisionByZeroError`, `ArgumentCountError` …). Your own exceptions extend them.
Methods: `getMessage()`, `getCode()`, `getPrevious()`, `getLine()`, `getFile()`, `__toString()`;
the constructor takes `($message, $code, $previous)`.

Inside `try`, runtime errors become `Error` objects that can be caught, e.g.
`catch (DivisionByZeroError $e)` for `1 / 0`. Without a matching `catch` the program stops as usual,
for exceptions with `Uncaught Exception: …` and the line where it was created. Exceptions also pass
through callbacks (`usort`, `array_map` …).

### Closures and callbacks

Functions are values too: `function` without a name creates a closure, `fn` an arrow function. A
closure only sees outside variables that it takes over with `use`, with their value at creation
time. An arrow function automatically takes every variable its expression uses. Inside methods
`$this` is available in both.

```php
$factor = 3;
$times = function ($x) use ($factor) { return $x * $factor; };
$plus = fn ($x) => $x + $factor;
echo $times(5), " ", $plus(5);                             // 15 8

$numbers = [5, 3, 8, 1];
print_r(array_map(fn ($x) => $x * $x, $numbers));          // [25, 9, 64, 1]
print_r(array_filter($numbers, fn ($x) => $x > 2));        // keys are kept
echo array_reduce($numbers, fn ($s, $x) => $s + $x, 0);    // 17
usort($players, fn ($a, $b) => $b['score'] <=> $a['score']);
```

Anything callable in PHP works as a callback: closures, function names (`'strtoupper'`,
`'my_function'`), `[$object, 'method']`, `['Class', 'method']` or `'Class::method'` and objects
with `__invoke()`. `$f(…)` calls such a value directly. `usort`, `uasort` and `uksort` sort stably;
the comparison may return a number or (like `$a > $b`) a boolean. As there are no references,
`use (&$x)` cannot write back and `array_walk()` can only read the array. Closures created in the
REPL stay callable as long as their variable exists.

### Text and input

```
cls()               clear the output
locate(x, y)        move the cursor to column x, row y
color(c)            text colour 0-15
input("TEXT")       shows TEXT and reads a line (the result is a string)
inkey()             last key typed on the on-screen keyboard, or ""
joy()               DS buttons held, as a bit mask
pause(n)            wait n frames (1/60 s), sleep(s) waits seconds
touch()             ['x' => 0-255, 'y' => 0-191] while the stylus is down, else null
swap_screens()      move the output to the touch screen and back; true/false sets it
```

`joy()` returns 1 = up, 2 = down, 4 = left, 8 = right, 16 = A, 32 = B, 64 = X, 128 = Y, 256 = L,
512 = R, with the constants `JOY_UP` … `JOY_R` (e.g. `if (joy() & JOY_UP) $y -= 2;`).
START remains the break button.

`swap_screens(true)` swaps the screens: text, graphics and sprites appear on the bottom touch
screen, the keyboard on top. That way the `touch()` coordinates match the graphics exactly, e.g. for
painting or buttons. During `input()` the keyboard temporarily comes back to the bottom. When the
program ends – normally, with an error, with START or `exit` – the screens are always back to normal.

```php
swap_screens(true);
while (!(joy() & JOY_B)) {
  if ($t = touch()) plot($t['x'], $t['y'], 2);
  pause(1);
}
```

### Date and time

`time()` reads the DS clock (set in the system settings; melonDS uses the PC's time). The DS clock
has no time zone, so timestamps count the seconds of local time since 1970.

```
time()                    current timestamp
date(f, $t)               timestamp as text, without $t the current time; format characters as in PHP:
                          d j m n Y y H G h g i s a A D l N w z W o F M t L S U c r, \x = character x
mktime(h, i, s, m, d, y)  timestamp from date and time (values out of range roll over,
                          missing values = now)
checkdate(m, d, y)        true when the date exists
```

```php
echo date("Y-m-d H:i"), PHP_EOL;                         // 2026-11-21 14:05
$newYearsEve = mktime(0, 0, 0, 12, 31, date("Y"));
echo intdiv($newYearsEve - time(), 86400), " days until New Year's Eve";
```

Timestamps are 32-bit numbers and last until January 2038.

## Graphics

Behind the text on the top screen there is a graphics layer of 256×192 pixels
(x 0–255, y 0–191). Colours are 0–15 as with `color()`, `-1` erases a pixel.
Colour `c` and fill `f` are optional (default colour via `gcolor()`).

```
gcls()                        clear graphics
gcolor(c)                     set the default drawing colour
plot(x, y[, c])               set a pixel
line(x1, y1, x2, y2[, c])     line
rect(x1, y1, x2, y2[, c[, f]])  rectangle, f = 1/true filled
circle(x, y, r[, c[, f]])     circle, f = 1/true filled
paint(x, y[, c])              flood-fill a connected area
point(x, y)                   colour of a pixel (-1 = empty)
```

Example:

```php
gcls(); cls();
for ($i = 0; $i < 16; $i++) circle(128, 96, 90 - $i * 5, $i, true);
line(0, 191, 255, 0, 2);
```

The graphics are visible on the output screen and hidden in the editor.

## Sprites

There are 256 sprite slots (0–255), each 8×8 pixels in the 16 colours or transparent.
They are shown as DS **hardware sprites**: 64 sprite objects (0–63) that sit above text and
graphics and can be moved without touching the background.

```
sprite(o, n, x, y[, s])   show or move sprite object o with image n at x,y, s = size 1-4
sproff([o])               hide object o, all objects without an argument
sprhit(a, b)              true if the visible objects a and b overlap
stamp(n, x, y[, s])       stamp image n into the graphics layer (s = 1-8), e.g. for backgrounds
sprset(n, x, y, c)        set a single pixel (x,y 0-7) of an image, c = -1 = transparent
SPRED [n]                 open the sprite editor (REPL command, otherwise menu ⋮ or X in the console)
```

Sprites are only visible on the output screen and may be partly off screen. `RUN` and `NEW`
hide all objects. When an image changes (editor or `sprset()`), every object using it is updated.

**Sprite editor** (menu ⋮, X in the REPL console or `SPRED n`): at the bottom an 8×8 grid to paint with the stylus,
a colour palette (checkerboard = transparent), a preview at 4×/2×/1× and buttons to change the slot,
copy, paste, clear and OK. The top screen shows an overview of all 256 slots; the current one
has a yellow frame. The d-pad changes the slot (←→ ±1, ↑↓ ±16), B/X/OK closes the editor.

## Sounds

There are 10 sound slots (0–9). Each slot is a tune of up to 64 notes with
a sound (square wave 12.5 %, 25 %, 50 % or noise) and a tempo.

```
sound(n)          play slot n in the background, the program keeps running
sound(n, true)    play slot n and wait until it has finished
sound(-1)         stop playback
SNDED [n]         open the sound editor (REPL command, otherwise menu ⋮ or Y in the console)
```

**Sound editor** (menu ⋮, Y in the REPL console or `SNDED n`): at the bottom a piano keyboard spanning two octaves. Tapping a
key plays the note and appends it to the tune with the selected length (1/16 to 1/1).
More buttons: rest (`REST`), delete the last note (`DEL`), clear the slot (`CLR`),
sound, tempo, octave, change slot, `PLAY`/`STOP` and `OK`. The top screen shows the notes of the
slot (e.g. `C#4  8` = eighth note) and an overview of all slots; during playback the
current note is highlighted. D-pad: ←→ slot, ↑↓ octave; B/Y/OK closes the editor.

### Tones and effects

Tones and noises also work straight from code, without the sound editor. They play on a voice
of their own, so also on top of a tune from `sound()`:

```
tone(f, ms, w)      tone f (Hz or a note name such as "C4", "F#5", "Eb3"), ms length (200),
                    w sound: WAVE_SQ12, WAVE_SQ25, WAVE_SQ50 (default), WAVE_NOISE
noise(ms, f)        noise
sfx("name")         ready-made effect, at once (cuts off queued tones):
                    blip select coin jump laser hit explosion powerup error
sound_busy()        true while a tune or a tone is still playing
sound(-1)           stop everything
```

Several `tone()` calls play one after the other in the background. When the queue is full the
program waits briefly until there is room again:

```php
foreach (["C4", "E4", "G4", "C5"] as $n) tone($n, 150);
while (sound_busy()) pause(1);
sfx("coin");
```

## Several files

A program can consist of several files. `main.php` always exists and is where `RUN` starts; other
files are pulled in as in PHP with `require "util.php";` (also `require_once`, `include`,
`include_once`). The included file is compiled at that point, its functions and variables are
available afterwards.

Above the keyboard a row of tabs shows all files of the program: tapping one opens it in the editor
(each file keeps its cursor position), `+` asks for the name of a new file in the REPL. With several
files, error messages name file and line (`Error in util.php:3: …`) and open the file at the error.

```
EDIT "util"        open or create util.php
FILES              all files of the program with their line counts
RENAME "new"       rename the open file (or RENAME "old" "new")
RMFILE "util"      remove a file from the program
```

File names: up to 24 characters from a–z, 0–9, `_`, `-`, the extension `.php` is added; at most
16 files per program. `main.php` cannot be renamed or removed.

```php
// main.php
require "util.php";
echo greet("DS");

// util.php
function greet($name) {
  return "Hello $name!";
}
```

### File browser

Tapping the red `<>` tile at the left of the tab row or `Open` in the `⋮` menu opens the file browser. It lists all programs on
the SD card (with their number of files); the top screen previews the selected program with its files
and the start of `main.php`.

| Button | Action |
|---|---|
| ↑ ↓ / d-pad / tap | select an entry (tap twice quickly = open or load) |
| `OPEN` (A) | show the files of the program, previewing the selected file |
| `LOAD` (START) | load the program; inside a program the selected file opens in the editor |
| `DELETE` | delete a program or a single file (tap twice; `main.php` stays) |
| `BACK` / `X` (B) | one level back or close the browser |

If the program in memory changed since the last `SAVE`/`LOAD`, `LOAD` asks before replacing it.

### Debugger

In the `⋮` menu on the right of the tab bar, `Debugger` replaces the keyboard with the debugger. It shows the code
of the file; tapping a line sets or clears a breakpoint there (red dot, red line number in the
editor). When the program reaches a breakpoint it stops before that line: the line is marked at the
bottom and the top screen lists the variables – first the locals of the running function (with
`$this`), then the globals, arrays and objects in short form.

| Button | Effect |
|---|---|
| `RUN` / `CONT` (START) | start the program or let it run on to the next breakpoint |
| `STEP` (A) | next line, also into called functions; without a running program: start and stop at the first line; while the program runs: pause it |
| `OVER` (X) | next line, function calls run through |
| `OUT` (Y) | run until the function returns to its caller |
| `STOP` | break the program |
| `ABC` (B) | back to the keyboard (breakpoints stay) |
| ↑ ↓ | scroll the code |
| L / R, SELECT | scroll the variables, switch the top screen between variables and output |

Breakpoints also work with a normal `RUN`; the debugger then opens by itself. When the program waits
for `input()`, the keyboard comes back. Code in callbacks of `usort`, `array_map` etc. runs through
without stopping.

## Examples

The [`examples/`](examples/) folder contains ready-made programs, each as a folder. To try them, copy
the folders to `/nds-php` on the SD card; they then appear in the list on the title screen
(or use `LOAD "PONG"`).

| Program | Contents | Controls |
|---|---|---|
| [PONG](examples/PONG/) | Pong against the computer, first to 7 points. Hardware sprites for paddles and ball, sounds for hits; `new_ball()` lives in `ball.php` and is pulled in with `require` | d-pad ↑↓ or W/S |
| [PAINT](examples/PAINT/) | Painting with the stylus: `swap_screens(true)` moves the output to the touch screen, `touch()` returns the position | tap a colour or X, Y clear, B quit |
| [SHAPES](examples/SHAPES/) | Random shapes with classes: interface `Shape`, abstract class `Figure` with trait `Colorful`, subclasses `Circle`, `Box`, `Square` in `shapes.php` | A new shapes, B quit |
| [SNAKE](examples/SNAKE/) | Snake with apples, grows longer and faster with every apple. Body drawn with `stamp()` into the graphics, head and apple as sprites, high score in `snake.json`, effects with `sfx()` | d-pad or W/A/S/D |

Sprites (`#SPR`) and sounds (`#SND`) are stored in `main.php`.

## Saving

Programs are saved on the SD card (flashcart via DLDI or DSi SD) under `/nds-php`, one folder per
program:

```
SAVE "GAME"       saves as /nds-php/GAME/main.php, util.php … (SAVE without a name = last name)
LOAD "GAME"       loads the program (replaces the current one)
DIR               lists all programs
DELETE "GAME"     deletes the program and its folder
```

Program names: up to 24 characters from A–Z, 0–9, `_`, `-`. Every file starts with `<?php` and holds
the lines exactly as in the editor (including indentation and blank lines), so it can be edited on
the PC. `SAVE` removes files from the folder that no longer belong to the program. Used sprites and
sounds are appended to `main.php` as PHP comments: `#SPR n` + 64 characters (`.` = transparent,
`0`–`F` = colour) and `#SND n sound tempo note:length …` (note as MIDI number, 0 = rest, length in
16ths). Older programs saved as a single file (`/nds-php/NAME.PHP`) can still be loaded.
Without an SD card the interpreter reports `NO SD CARD` at start-up. Programs of older versions
(folder `/nds-basic`) are left alone; copy them to `/nds-php` to use them here.

In the melonDS emulator: enable SD emulation under *Config → Emu settings → DLDI*
(optionally with "Sync SD to folder").

## Files and JSON

Programs can read and write files of their own, such as high scores or save games. They live in
the program's folder (`/nds-php/GAME/`). A program that has not been saved yet writes straight
to `/nds-php/`. File names consist of letters, digits, `_`, `-` and `.` (no folders, max.
64 characters); `.php` files belong to the editor and are read-only.

```
file_put_contents(f, $s)    write a file, returns the bytes written or false
                            (3rd argument FILE_APPEND: append; $s may be an array)
file_get_contents(f)        contents as a string, false when missing
file(f, flags)              lines as an array (FILE_IGNORE_NEW_LINES, FILE_SKIP_EMPTY_LINES)
file_exists(f)              true when the file exists
unlink(f)                   delete the file
json_encode($v, flags)      value as JSON (JSON_PRETTY_PRINT, JSON_UNESCAPED_SLASHES)
json_decode($s)             parse JSON, null on errors; JSON objects always become arrays
```

```php
$best = json_decode(file_get_contents("score.json"))["best"] ?? 0;
if ($points > $best) file_put_contents("score.json", json_encode(["best" => $points]));
```

## Website

The landing page lives in [`site/`](site/), in English (`index.html`, `docs.html`) and German
(`index.de.html`, `docs.de.html`) with a language switch in the header. The workflow
[`.github/workflows/pages.yml`](.github/workflows/pages.yml) builds the ROM in the devkitPro container
on every push to `main`, assembles the site with `nds-php.nds`, this README and the German one
(as documentation pages) and the examples, and publishes it via GitHub Pages.

One-time repository setting: *Settings → Pages → Build and deployment → Source: GitHub Actions*.

To view it locally:

```bash
./build.sh && sh site/build-site.sh && python3 -m http.server -d _site 8000
```
