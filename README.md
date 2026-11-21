# NDS PHP

*[English](README.en.md) · Deutsch*

Ein kleiner, retro-inspirierter PHP-Interpreter für den Nintendo DS – programmiert wird wie früher
direkt auf dem Gerät, wie früher am Heimcomputer, aber in einem PHP-Dialekt.

* **Oberer Bildschirm:** Code-Editor mit Zeilennummern und Statuszeile bzw. die REPL-Konsole
  mit der Programmausgabe
* **Unterer Bildschirm:** Touch-Tastatur mit Statusanzeige; die Oberfläche ist hell gehalten, mit Rot als Akzentfarbe
* **Titelbildschirm:** Beim Start regnen oben die Buchstaben „NDS PHP“ als 3D-Voxel herab und bilden
  das Logo, gerendert mit der 3D-Hardware des DS. Unten listet er alle gespeicherten Programme
  (12 pro Seite, mit ← → blättern): Antippen lädt das Programm direkt in den Editor. „NEUES PROGRAMM“
  bzw. „ZUM PROGRAMM“ oder eine beliebige Taste startet den Editor ohne zu laden.
  `EXIT` (in der REPL) kehrt zum Titelbildschirm zurück, Programm und Daten bleiben erhalten

## Bauen

Benötigt nur Docker (Image `devkitpro/devkitarm`):

```bash
./build.sh          # erzeugt nds-php.nds
./build.sh clean
```

Interpreter-Tests auf dem PC (ohne DS):

```bash
make -C host test test-files test-examples
```

## Bedienung

Der obere Bildschirm ist entweder der **Code-Editor** oder die **REPL-Konsole**; `SCRN` wechselt
zwischen beiden.

| Aktion | Tastatur | Tasten |
|---|---|---|
| Programm starten / abbrechen | `RUN` / `STOP` | START |
| Zwischen Editor und REPL-Konsole wechseln | `SCRN` | – |
| Hilfeseite mit allen Befehlen (↑↓ scrollen, ←→ blättern) | – | SELECT |
| Cursor bewegen | `←` `→` `↑` `↓` | Steuerkreuz |
| Zeilenanfang / -ende, seitenweise blättern | – | R + ←→, R + ↑↓ (in der Konsole auch L) |
| Neue Zeile (Einrückung wird übernommen) | `RET` | A |
| Zeichen links vom Cursor löschen (am Zeilenanfang: Zeilen verbinden) | `DEL` | B |
| Ganze Zeile löschen / ausschneiden | `CLR` | X (im Editor) |
| Zeile duplizieren (Kopie darunter, Cursor auf der Kopie) | – | Y (im Editor) |
| Großbuchstaben und Zweitbelegung (2× tippen = Lock) | `SHFT` | R halten (in der Konsole auch L) |
| Code auf den Touchscreen (markieren, umbenennen …) | – | L halten / antippen (im Editor) |
| Einfügen / rückgängig / wiederholen | – | SELECT + X / Y / A (im Editor) |
| Menü `⋮` (rechts in der Reiterleiste): Öffnen (Dateibrowser), Speichern, Debugger, Sprite-Editor, Sound-Editor, Exit (zum Titelbild) | – | – |
| Sprite-Editor | – | X (in der REPL-Konsole) |
| Sound-Editor | – | Y (in der REPL-Konsole) |

Die Tastatur tippt Kleinbuchstaben; mit Shift kommen Großbuchstaben und die für PHP nötigen Zeichen
`\ { } [ ] | : *` (jeweils oben links auf der Taste).

**Editor:** Der Code wird direkt bearbeitet – einfach den Cursor hinsetzen und tippen. Die
Zeilennummern am linken Rand dienen nur der Orientierung (und den Fehlermeldungen), eingeben muss
man sie nicht. Lange Zeilen werden umbrochen. Nach einer Zeile, die mit `{` endet, rückt `RET` zwei
Zeichen weiter ein, eine `}` auf einer sonst leeren Zeile rückt wieder aus. Bricht ein Programm mit
einem Fehler ab, steht der Cursor beim Zurückwechseln in der Fehlerzeile.

**Code auf dem Touchscreen (Refactoring):** Im Editor **L halten** tauscht die Bildschirme: Der
Code kommt nach unten auf den Touchscreen, die Tastatur nach oben. **L kurz antippen** tauscht
dauerhaft, bis man L noch einmal antippt (oder „Fertig“). Mit dem Stift:

| Geste | Wirkung |
|---|---|
| Tipp auf ein Wort | Wort markieren (`$x` samt `$`) |
| Tipp daneben | nur den Cursor setzen |
| senkrecht ziehen | ganze Zeilen markieren (am oberen / unteren Rand wird weitergescrollt) |
| waagrecht in einer Zeile ziehen | Text in der Zeile markieren |
| über die Zeilennummern wischen | scrollen |

Unter dem Code steht eine Leiste mit Knöpfen, je nachdem was markiert ist:

* **Wort:** *Umben.* (umbenennen), *Def.* (zur Definition springen), *Verw.* (alle Verwendungen
  als Liste, ein Tipp springt hin), *Kopie*, *Aus* (ausschneiden)
* **Zeilen:** *Aus*, *Kopie*, *Dupl*, `«` `»` aus- / einrücken, `↑` `↓` Block verschieben
* **Text in einer Zeile:** *Aus*, *Kopie*
* **nichts:** *Einf.* (einfügen), *Rückg.*, *Wdh.*

**Umbenennen** zeigt auf dem oberen Bildschirm alle Stellen in allen Dateien mit dem neuen Namen in
Grün; Treffer in Strings und Kommentaren erscheinen grau und bleiben unverändert. Neuen Namen
tippen, `RET` benennt um, `SCRN` bricht ab. Was dazugehört, ergibt sich aus der Stelle:

| Wort | umbenannt wird |
|---|---|
| `$x` in einer Funktion | nur in dieser Funktion (Closures und `fn` darin gehören dazu) |
| `$x` außerhalb von Funktionen | im globalen Code aller Dateien, auch in `"…$x…"` |
| Funktion / Konstante | Deklaration und alle Aufrufe |
| Klasse, Interface, Trait, Enum | Deklaration, `new`, `extends`, Typen, `X::`, `catch` … |
| Methode, Eigenschaft, Konstante, Enum-Fall | Deklaration und alle `->name` / `::name` |

Methoden und Eigenschaften werden nur am Namen erkannt: Gibt es den Namen in zwei Klassen, werden
beide umbenannt. Eingebaute Funktionen und Klassen lassen sich nicht umbenennen.

Die markierten Zeilen bleiben nach dem Zurücktauschen erhalten: **X** schneidet sie aus, **Y**
dupliziert sie. Ohne Markierung schneidet X die aktuelle Zeile aus (sie landet in der
Zwischenablage). **SELECT+X** fügt ein, **SELECT+Y** macht rückgängig, **SELECT+A** wiederholt (die
letzten 20 Änderungen, ein Umbenennen über mehrere Dateien zählt als eine). Im Editor ist dafür
nur noch **R** die Shift-Taste.

**Autovervollständigung:** Beim Tippen im Editor und an der REPL erscheinen über der Tastatur
antippbare Vorschläge, darunter die Signatur des ersten (bei eingebauten Funktionen aus der Hilfe,
bei eigenen mit ihren Parametern). Ein Tipp übernimmt den Vorschlag; Funktionen bekommen gleich ihre
`(`. Die R-Schultertaste allein gedrückt und losgelassen übernimmt den ersten Vorschlag, der Pfeil
rechts blättert weiter. Was vorgeschlagen wird, hängt vom Text vor dem Cursor ab:

| Vor dem Cursor | Vorschläge |
|---|---|
| ein Wort ab 2 Buchstaben | eigene und eingebaute Funktionen, Klassen, Schlüsselwörter, Konstanten, Snippets |
| `$…` | Variablen des Programms |
| `->…` | Methoden und Eigenschaften der Klassen des Programms |
| `Klasse::…` | Konstanten, Enum-Fälle und Methoden dieser Klasse, bei Enums `cases`/`from`/`tryFrom` |
| `new `, `instanceof `, `catch (`, `extends ` | eigene Klassen und die eingebauten Exceptions |

Snippets setzen ganze Konstrukte ein und stellen den Cursor an die richtige Stelle: `fe` →
`foreach ($ as $v) {`, `fo` → `for`, `wh` → `while`, `if`, `sw` → `switch`, `ma` → `match`, `fn` →
`function () {`, `tc` → `try { } catch (Exception $e) { }`, `cl` → `class  {`, `co` →
`public function __construct(public $) {}` (Property Promotion), `pf` → `public function () {`. Zuletzt übernommene Vorschläge stehen
vorn, danach eigene Namen vor eingebauten. In Strings und Kommentaren gibt es keine Vorschläge.

**REPL-Konsole:** Sie zeigt die Ausgabe der Programme und hat einen Prompt `>`, an dem man einzelne
PHP-Anweisungen sofort ausführt (`echo 2 + 3;`, `$x = strlen("abc");`) oder Befehle wie `RUN`,
`SAVE "NAME"` und `LOAD "NAME"` eingibt. Ein Ausdruck ohne `echo` zeigt sein Ergebnis gleich an
(`1 + 2` → `= 3`, `strtoupper("ds")` → `= "DS"`, Arrays kurz als `[1, 2]`, lange Arrays und Objekte mit einem Element pro Zeile, alles farbig wie im
Editor); Zuweisungen, `null` und
Funktionen, die selbst ausgeben (`printf`, `print_r` …), bleiben still. Die Eingabe wird wie im Editor farbig hervorgehoben, Befehle wie `RUN` erscheinen
rot. ↑↓ holen frühere Eingaben zurück. `START` bzw. `RUN`
starten das Programm aus dem Editor heraus und schalten dafür auf die Konsole.

**Sprache:** Die Oberfläche (Titelbildschirm, Hinweise, Hilfeseite) richtet sich nach der Systemsprache des DS.
Ist der DS auf Deutsch eingestellt, erscheint sie auf Deutsch, bei jeder anderen Sprache auf Englisch.
Fehlermeldungen und Befehle sind immer englisch.

## Der PHP-Dialekt

`RUN` fügt alle Zeilen in ihrer Reihenfolge zu einem Programm zusammen, übersetzt es in Bytecode und
führt ihn aus. Blöcke mit `{ }` dürfen sich also über beliebig viele Zeilen erstrecken, `<?php` am
Anfang ist erlaubt, aber nicht nötig. Fehler werden mit der Zeilennummer gemeldet
(`Parse error in line 3: unexpected 'echo'`, `Error in line 2: Division by zero`).

```php
function fib($n) {
  return $n < 2 ? $n : fib($n - 1) + fib($n - 2);
}
for ($i = 0; $i < 10; $i++) echo fib($i), " ";
$name = input("NAME? ");
echo "\nHALLO $name!";
```

Unterstützt werden:

* **Werte:** `int` (32 Bit), `float`, Strings, `true`/`false`/`null`, Arrays als geordnete Maps mit
  Zahlen- oder Text-Schlüsseln (`[1, 2]`, `['a' => 1]`, `array(…)`, `$a[] = 3`), Arrays werden wie in
  PHP als Wert kopiert
* **Strings:** `'…'` und `"…"` mit `\n \t \\ \" \$` und Variablen (`"Hallo $name"`, `"$a[0]"`,
  `"{$a['x']['y']}"`)
* **Operatoren:** `+ - * / % **`, `.`, `== != === !== < > <= >= <=>`, `&& || ! and or xor`,
  `& | ^ ~ << >>`, `?:`, `??`, `= += -= *= /= .= %= **= &= |=`, `++ --`, Casts `(int) (float) (string) (bool) (array)`
* **Anweisungen:** `echo`, `print`, `if/elseif/else`, `while`, `do…while`, `for`, `foreach ($a as $k => $v)`,
  `switch/case/default`, `break n`, `continue n`, `function name($a, $b = 1) { … return …; }`,
  `global`, `unset`, `isset`, `empty`, `exit`/`die`, Kommentare `//`, `#`, `/* */`
* **Funktionen:** `strlen substr strpos str_repeat str_pad str_replace strcmp strcasecmp str_contains str_starts_with
  str_ends_with strtoupper strtolower ucfirst lcfirst trim ltrim rtrim strrev explode implode sprintf
  printf vsprintf vprintf number_format chr ord intval floatval strval boolval is_numeric is_int is_float is_string
  is_bool is_array is_null gettype get_debug_type abs floor ceil round sqrt pow sin cos tan asin acos atan atan2
  exp log log10 pi fmod intdiv deg2rad rad2deg min max rand mt_rand srand count in_array
  array_search array_key_exists array_keys array_values array_sum array_reverse array_slice
  array_merge array_fill range array_push array_pop array_shift array_unshift sort rsort shuffle
  print_r var_dump time date mktime checkdate`, mit Callbacks: `array_map array_filter array_reduce array_walk usort uasort uksort
  call_user_func call_user_func_array is_callable function_exists`
* **Konstanten:** `PHP_EOL`, `PHP_INT_MAX`, `M_PI`, `STR_PAD_LEFT/RIGHT/BOTH`, `JOY_UP` … `JOY_R`,
  eigene mit `const NAME = wert;`
* **Closures und Arrow Functions** (`function () use ($x) { … }`, `fn ($x) => …`) – siehe unten
* **Klassen, Interfaces und Traits** – siehe unten
* **Exceptions** (`try`/`catch`/`finally`, `throw`) – siehe unten
* **PHP 8:** `match`, `enum`, Named Arguments, Typangaben und `readonly` – siehe unten

Nicht dabei sind u. a. Referenzen (`&`), statische Variablen in Funktionen und
Namespaces (ein führendes `\` wie in `\Exception` ist erlaubt). Schlüsselwörter und Funktionsnamen sind wie in PHP unabhängig von Groß-/Kleinschreibung,
Variablen nicht. Zahlen mit Komma sind auf dem DS `float` mit einfacher Genauigkeit.

In der **REPL** bleiben die Variablen und Funktionen des letzten `RUN` erhalten
(`echo $punkte;`, `echo fib(12);`). `CLR` löscht alle Variablen.

### Klassen, Interfaces und Traits

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

* **Klassen:** `class`, `extends`, `implements`, `abstract`, `final`, Eigenschaften mit
  Standardwert (auch Arrays), Methoden, `const`, `static` Eigenschaften und Methoden,
  Sichtbarkeit `public`/`protected`/`private` (wird geprüft), Konstruktor `__construct()` mit
  Promotion (`public function __construct(private int $x)`), Typangaben werden geprüft –
  siehe „Typen“
* **Objekte:** `new A(…)`, `new static`/`new self`, `$this`, `$o->x`, `$o->f()`, Ketten wie
  `$this->items[] = $x` oder `(new A)->f()->g()`, `$o->$name`, `A::X`, `A::$n`, `A::f()`,
  `self::`, `parent::`, `static::` (späte statische Bindung), `A::class`, `instanceof`, `clone`,
  `__toString()` für `echo`, Verkettung und Strings, `foreach` über die sichtbaren Eigenschaften,
  `print_r`/`var_dump`. Objekte werden wie in PHP geteilt, nicht kopiert
* **Interfaces:** Methoden ohne Rumpf und Konstanten, `extends` mehrerer Interfaces; beim `RUN`
  wird geprüft, dass konkrete Klassen alle Methoden implementieren
* **Traits:** Eigenschaften und Methoden, per `use A, B;` in Klassen eingemischt; jede Klasse
  bekommt eigene statische Eigenschaften. Konfliktauflösung (`insteadof`, `as`) gibt es nicht
* **Funktionen:** `get_class`, `get_parent_class`, `get_object_vars`, `method_exists`,
  `property_exists`, `class_exists`, `interface_exists`, `is_object`

Klassen werden im Programm deklariert (nicht in der REPL), dürfen aber in jeder Datei stehen und
vor oder nach ihrer Verwendung kommen. Andere magische Methoden als `__construct` und
`__toString` gibt es nicht.

### match, enum und Named Arguments

`match` vergleicht streng (`===`), liefert einen Wert und kennt mehrere Bedingungen pro Zweig.
Passt nichts und fehlt `default`, gibt es einen `UnhandledMatchError`:

```php
echo match ($note) { 1, 2 => "gut", 3, 4 => "ok", default => "naja" };
$text = match (true) { $temp < 0 => "Frost", $temp < 20 => "frisch", default => "warm" };
```

Enums gibt es als reine und als hinterlegte Variante (`int` oder `string`), mit Methoden,
Konstanten, statischen Methoden und Interfaces. Jeder Fall ist ein Objekt, das es nur einmal gibt
(`===` vergleicht also Fälle):

```php
enum Farbe: string {
  case Rot = 'r';
  case Gruen = 'g';
  public function hell(): bool { return $this === self::Gruen; }
}
$f = Farbe::from('r');                  // Farbe::Rot, tryFrom() liefert null statt Fehler
echo $f->name, " ", $f->value;          // Rot r
foreach (Farbe::cases() as $c) echo $c->name;
echo match ($f) { Farbe::Rot => "stopp", Farbe::Gruen => "los" };
```

`name` und `value` sind nur lesbar; Enums lassen sich nicht mit `new` erzeugen. `json_encode()`
schreibt bei hinterlegten Enums den Wert. Alle Enums erfüllen `UnitEnum`, hinterlegte zusätzlich
`BackedEnum`.

**Named Arguments** übergeben Werte per Parametername, auch gemischt mit normalen Argumenten davor;
ausgelassene optionale Parameter behalten ihren Standardwert. Das geht bei eigenen Funktionen,
Methoden, Konstruktoren und Closures (nicht bei eingebauten Funktionen):

```php
function karte(string $farbe = 'H', int $wert = 1, bool $bild = false) { ... }
karte(wert: 12, bild: true);
$p = new Punkt(y: 5);
```

### Typen

Typangaben werden wie in PHP 8 zur Laufzeit geprüft: bei Parametern, Rückgabewerten und
Eigenschaften. Erlaubt sind `int`, `float`, `string`, `bool`, `array`, `callable`, `iterable`,
`object`, `mixed`, `null`, `false`, `true`, Klassen- und Interface-Namen, `self`/`static`/`parent`,
nullable (`?int`) und Union-Typen (`int|string|null`), bei Rückgaben auch `void` und `never`.

```php
function plus(int $a, int $b): int { return $a + $b; }
echo plus(2, "3");                     // 5: "3" wird zu 3
plus("abc", 1);                        // TypeError: plus(): Argument #1 ($a) must be of type int, string given

class Punkt {
  public function __construct(public int $x = 0, public readonly ?string $name = null) {}
}
$p = new Punkt(x: "5");                 // $p->x ist int(5)
$p->x = "zehn";                         // TypeError: Cannot assign string to property Punkt::$x of type int
$p->name = "neu";                       // Error: Cannot modify readonly property Punkt::$name
```

* **Umwandlung:** Ohne Weiteres wandelt PHP passende Skalare um (`"5"` → `5`, `5` → `"5"`,
  `1` → `true`). Mit `declare(strict_types=1);` muss der Typ genau passen, nur `int` wird noch zu
  `float`. Hier gilt die Angabe für das ganze Programm, nicht nur für die eine Datei.
* **Eigenschaften:** `public int $n = 0;` prüft jede Zuweisung. Eine typisierte Eigenschaft ohne
  Standardwert ist zunächst nicht initialisiert. Lesen gibt dann einen Fehler, `isset()` und `??`
  liefern `false` bzw. den Ersatzwert.
* **`readonly`:** Die Eigenschaft wird einmal innerhalb der Klasse gesetzt (meist im Konstruktor)
  und ist danach nur lesbar. `readonly class` macht alle Eigenschaften `readonly`. Promotion im
  Konstruktor übernimmt Typ und `readonly`.
* **Fehler:** Falsche Typen werfen einen `TypeError`, den man mit `catch` fangen kann. Fehler, die
  sich schon beim Übersetzen erkennen lassen, kommen als Parse-Fehler, z. B. `return 1;` in einer
  `void`-Funktion oder ein falscher Standardwert.
* **Hilfsfunktionen:** `gettype()` und `get_debug_type()` (`int`, `float`, `Punkt`, `Closure` …).
* **Nicht dabei:** Intersection-Typen (`A&B`), Typprüfung bei eingebauten Funktionen und bei
  statischen Eigenschaften, der Vergleich der Signaturen beim Überschreiben von Methoden.

### Exceptions

`try`, `catch` und `finally` funktionieren wie in PHP 8, auch mit mehreren Klassen
(`catch (A | B $e)`), ohne Variable (`catch (Exception)`) und `throw` als Ausdruck
(`$x ?? throw new Exception("leer")`).

```php
function teile($a, $b) {
  if ($b == 0) throw new InvalidArgumentException("Durch null geteilt", 1);
  return $a / $b;
}
try {
  echo teile(10, 0);
} catch (InvalidArgumentException $e) {
  echo "Fehler in Zeile ", $e->getLine(), ": ", $e->getMessage();
} finally {
  echo "\nfertig";
}
```

Eingebaut sind `Throwable`, `Exception` und `Error` mit den üblichen Unterklassen
(`RuntimeException`, `LogicException`, `InvalidArgumentException`, `OutOfRangeException`,
`UnexpectedValueException`, `TypeError`, `DivisionByZeroError`, `ArgumentCountError` …). Eigene
Exceptions erben davon. Methoden: `getMessage()`, `getCode()`, `getPrevious()`, `getLine()`,
`getFile()`, `__toString()`; der Konstruktor nimmt `($message, $code, $previous)`.

Laufzeitfehler werden innerhalb von `try` zu `Error`-Objekten und lassen sich fangen, etwa
`catch (DivisionByZeroError $e)` bei `1 / 0`. Ohne passendes `catch` bricht das Programm wie gewohnt
ab, bei Exceptions mit `Uncaught Exception: …` und der Zeile, in der sie erzeugt wurde.
Exceptions laufen auch durch Callbacks hindurch (`usort`, `array_map` …).

### Closures und Callbacks

Funktionen sind auch Werte: `function` ohne Namen erzeugt eine Closure, `fn` eine Arrow Function.
Eine Closure sieht Variablen von außen nur, wenn sie mit `use` übernommen werden, und zwar mit dem
Wert beim Erzeugen. Eine Arrow Function übernimmt automatisch alle Variablen, die ihr Ausdruck
benutzt. In Methoden ist `$this` in beiden verfügbar.

```php
$faktor = 3;
$mal = function ($x) use ($faktor) { return $x * $faktor; };
$plus = fn ($x) => $x + $faktor;
echo $mal(5), " ", $plus(5);                               // 15 8

$zahlen = [5, 3, 8, 1];
print_r(array_map(fn ($x) => $x * $x, $zahlen));           // [25, 9, 64, 1]
print_r(array_filter($zahlen, fn ($x) => $x > 2));         // Schlüssel bleiben erhalten
echo array_reduce($zahlen, fn ($s, $x) => $s + $x, 0);     // 17
usort($spieler, fn ($a, $b) => $b['punkte'] <=> $a['punkte']);
```

Als Callback geht alles, was in PHP aufrufbar ist: Closures, Funktionsnamen (`'strtoupper'`,
`'meine_funktion'`), `[$objekt, 'methode']`, `['Klasse', 'methode']` bzw. `'Klasse::methode'` und
Objekte mit `__invoke()`. `$f(…)` ruft einen solchen Wert direkt auf. `usort`, `uasort` und
`uksort` sortieren stabil; die Vergleichsfunktion darf eine Zahl oder (wie `$a > $b`) einen
Wahrheitswert liefern. Da es keine Referenzen gibt, kann `use (&$x)` nichts zurückschreiben, und
`array_walk()` kann das Array nur lesen. Closures, die in der REPL entstehen, bleiben aufrufbar,
solange die Variable existiert.

### Text und Eingabe

```
cls()               Ausgabe löschen
locate(x, y)        Cursor auf Spalte x, Zeile y
color(c)            Textfarbe 0-15
input("TEXT")       zeigt TEXT und liest eine Zeile ein (Ergebnis ist ein String)
inkey()             zuletzt getippte Taste der Bildschirmtastatur oder ""
joy()               gehaltene DS-Tasten als Bitmaske
pause(n)            n Frames (1/60 s) warten, sleep(s) wartet Sekunden
touch()             ['x' => 0-255, 'y' => 0-191], solange der Stylus aufliegt, sonst null
swap_screens()      Ausgabe auf den Touchscreen legen bzw. zurück; true/false legt es fest
```

`joy()` liefert 1 = hoch, 2 = runter, 4 = links, 8 = rechts, 16 = A, 32 = B, 64 = X, 128 = Y, 256 = L,
512 = R, dafür gibt es die Konstanten `JOY_UP` … `JOY_R` (z. B. `if (joy() & JOY_UP) $y -= 2;`).
START bleibt die Abbruchtaste.

Mit `swap_screens(true)` tauschen die Bildschirme: Text, Grafik und Sprites erscheinen unten auf dem
Touchscreen, die Tastatur oben. So passen die `touch()`-Koordinaten genau zur Grafik, etwa zum Malen
oder für Knöpfe. Während `input()` kommt die Tastatur vorübergehend wieder nach unten. Endet das
Programm – normal, durch einen Fehler, mit START oder `exit` –, sind die Bildschirme immer wieder in
der Ausgangslage.

```php
swap_screens(true);
while (!(joy() & JOY_B)) {
  if ($t = touch()) plot($t['x'], $t['y'], 2);
  pause(1);
}
```

### Datum und Uhrzeit

`time()` liest die Uhr des DS (in den Systemeinstellungen gestellt, in melonDS die Uhrzeit des PCs).
Die DS-Uhr kennt keine Zeitzone, daher zählen die Zeitstempel die Sekunden der Ortszeit seit 1970.

```
time()                    aktueller Zeitstempel
date(f, $t)               Zeitstempel als Text, ohne $t die aktuelle Zeit; Formatzeichen wie in PHP:
                          d j m n Y y H G h g i s a A D l N w z W o F M t L S U c r, \x = Zeichen x
mktime(h, i, s, m, d, y)  Zeitstempel aus Datum und Uhrzeit (zu große Werte rollen über,
                          fehlende Werte = jetzt)
checkdate(m, d, y)        true, wenn es das Datum gibt
```

```php
echo date("d.m.Y H:i"), PHP_EOL;                         // 21.11.2026 14:05
$silvester = mktime(0, 0, 0, 12, 31, date("Y"));
echo "Noch ", intdiv($silvester - time(), 86400), " Tage bis Silvester";
```

Tag- und Monatsnamen (`D l M F`) sind wie in PHP englisch. Zeitstempel sind 32-Bit-Zahlen und reichen
bis Januar 2038.

## Grafik

Auf dem oberen Bildschirm liegt hinter dem Text eine Grafikebene mit 256×192 Punkten
(x 0–255, y 0–191). Farben sind 0–15 wie bei `color()`, `-1` löscht einen Punkt.
Farbe `c` und Füllung `f` sind optional (Standardfarbe über `gcolor()`).

```
gcls()                        Grafik löschen
gcolor(c)                     Standard-Zeichenfarbe setzen
plot(x, y[, c])               Punkt setzen
line(x1, y1, x2, y2[, c])     Linie
rect(x1, y1, x2, y2[, c[, f]])  Rechteck, f = 1/true gefüllt
circle(x, y, r[, c[, f]])     Kreis, f = 1/true gefüllt
paint(x, y[, c])              zusammenhängende Fläche füllen
point(x, y)                   Farbe eines Punktes (-1 = leer)
```

Beispiel:

```php
gcls(); cls();
for ($i = 0; $i < 16; $i++) circle(128, 96, 90 - $i * 5, $i, true);
line(0, 191, 255, 0, 2);
```

Die Grafik ist auf dem Ausgabebildschirm sichtbar, im Editor wird sie ausgeblendet.

## Sprites

Es gibt 256 Sprite-Slots (0–255) mit je 8×8 Punkten in den 16 Farben oder transparent.
Angezeigt werden sie als **Hardware-Sprites** des DS: 64 Sprite-Objekte (0–63), die über
Text und Grafik liegen und sich bewegen lassen, ohne den Hintergrund zu verändern.

```
sprite(o, n, x, y[, s])   Sprite-Objekt o mit Bild n an x,y zeigen bzw. bewegen, s = Größe 1-4
sproff([o])               Objekt o ausblenden, ohne Angabe alle
sprhit(a, b)              true, wenn sich die sichtbaren Objekte a und b überlappen
stamp(n, x, y[, s])       Bild n in die Grafikebene stempeln (s = 1-8), z. B. für Hintergründe
sprset(n, x, y, c)        einzelnen Punkt (x,y 0-7) eines Bildes setzen, c = -1 = transparent
SPRED [n]                 Sprite-Editor öffnen (REPL-Befehl, sonst Menü ⋮ oder X in der Konsole)
```

Sprites sind nur auf dem Ausgabebildschirm sichtbar und dürfen teilweise außerhalb des
Bildschirms liegen. `RUN` und `NEW` blenden alle Objekte aus. Ändert man ein Bild (Editor oder
`sprset()`), aktualisieren sich alle Objekte, die es benutzen.

**Sprite-Editor** (Menü ⋮, X in der REPL-Konsole oder `SPRED n`): unten ein 8×8-Raster zum Malen mit dem Stylus,
Farbpalette (Karo = transparent), Vorschau in 4×/2×/1×, Knöpfe für Slot wechseln, Kopieren,
Einfügen, Leeren und OK. Oben steht eine Übersicht aller 256 Slots; der aktuelle ist gelb
umrahmt. Steuerkreuz wechselt den Slot (←→ ±1, ↑↓ ±16), B/X/OK schließt den Editor.

## Sounds

Es gibt 10 Sound-Slots (0–9). Jeder Slot ist eine Melodie aus bis zu 64 Noten mit
Klang (Rechteck 12,5 %, 25 %, 50 % oder Rauschen) und Tempo.

```
sound(n)          Slot n im Hintergrund abspielen, das Programm läuft weiter
sound(n, true)    Slot n abspielen und warten, bis er fertig ist
sound(-1)         Wiedergabe stoppen
SNDED [n]         Sound-Editor öffnen (REPL-Befehl, sonst Menü ⋮ oder Y in der Konsole)
```

**Sound-Editor** (Menü ⋮, Y in der REPL-Konsole oder `SNDED n`): unten eine Klaviatur über zwei Oktaven. Ein Tipp
auf eine Taste spielt den Ton und hängt ihn mit der gewählten Länge (1/16 bis 1/1) an die
Melodie an. Weitere Knöpfe: Pause (`REST`), letzte Note löschen (`DEL`), Slot leeren (`CLR`),
Klang, Tempo, Oktave, Slot wechseln, `PLAY`/`STOP` und `OK`. Oben stehen die Noten des
Slots (z. B. `C#4  8` = Achtelnote) und eine Übersicht aller Slots; beim Abspielen wird die
aktuelle Note markiert. Steuerkreuz: ←→ Slot, ↑↓ Oktave; B/Y/OK schließt den Editor.

### Töne und Effekte

Ohne Sound-Editor gehen Töne und Geräusche auch direkt aus dem Code. Sie laufen auf einer
eigenen Stimme, also auch über einer Melodie aus `sound()`:

```
tone(f, ms, w)      Ton f (Hz oder Notenname wie "C4", "F#5", "Eb3"), ms Länge (200),
                    w Klang: WAVE_SQ12, WAVE_SQ25, WAVE_SQ50 (Standard), WAVE_NOISE
noise(ms, f)        Rauschen
sfx("name")         fertiger Effekt, sofort (bricht laufende Töne ab):
                    blip select coin jump laser hit explosion powerup error
sound_busy()        true, solange noch eine Melodie oder ein Ton spielt
sound(-1)           alles stoppen
```

Mehrere `tone()`-Aufrufe werden nacheinander im Hintergrund gespielt. Ist die Warteschlange
voll, wartet das Programm kurz, bis wieder Platz ist:

```php
foreach (["C4", "E4", "G4", "C5"] as $n) tone($n, 150);
while (sound_busy()) pause(1);
sfx("coin");
```

## Mehrere Dateien

Ein Programm kann aus mehreren Dateien bestehen. `main.php` ist immer da und der Startpunkt von
`RUN`; weitere Dateien bindet man wie in PHP mit `require "util.php";` ein (auch `require_once`,
`include`, `include_once`). Die eingebundene Datei wird an dieser Stelle übersetzt, ihre Funktionen
und Variablen stehen danach zur Verfügung.

Über der Tastatur zeigt eine Reiterleiste alle Dateien des Programms: Antippen öffnet die Datei im
Editor (jede Datei merkt sich ihre Cursorposition), `+` fragt in der REPL nach dem Namen einer neuen
Datei. Fehlermeldungen nennen bei mehreren Dateien Datei und Zeile (`Error in util.php:3: …`) und
öffnen die Datei an der Fehlerzeile.

```
EDIT "util"        Datei util.php öffnen bzw. anlegen
FILES              alle Dateien des Programms mit Zeilenzahl
RENAME "neu"       geöffnete Datei umbenennen (oder RENAME "alt" "neu")
RMFILE "util"      Datei aus dem Programm entfernen
```

Dateinamen: bis 24 Zeichen aus a–z, 0–9, `_`, `-`, die Endung `.php` wird ergänzt; höchstens
16 Dateien pro Programm. `main.php` kann nicht umbenannt oder entfernt werden.

```php
// main.php
require "util.php";
echo greet("DS");

// util.php
function greet($name) {
  return "Hallo $name!";
}
```

### Dateibrowser

Ein Tipp auf das rote `<>`-Symbol links in der Reiterleiste oder `Öffnen` im Menü `⋮` öffnet den Dateibrowser. Er listet alle
Programme auf der SD-Karte (mit Anzahl der Dateien); oben erscheint eine Vorschau des ausgewählten
Programms mit seinen Dateien und dem Anfang von `main.php`.

| Knopf | Wirkung |
|---|---|
| ↑ ↓ / Steuerkreuz / Tippen | Eintrag auswählen (zweimal schnell tippen = öffnen bzw. laden) |
| `ÖFFNEN` (A) | Dateien des Programms zeigen, mit Vorschau der gewählten Datei |
| `LADEN` (START) | Programm laden; innerhalb eines Programms wird die gewählte Datei im Editor geöffnet |
| `LÖSCHEN` | Programm bzw. einzelne Datei löschen (zweimal tippen; `main.php` bleibt) |
| `ZURÜCK` / `X` (B) | eine Ebene zurück bzw. Browser schließen |

Ist das Programm im Speicher seit dem letzten `SAVE`/`LOAD` geändert, fragt `LADEN` vor dem Ersetzen
nach.

### Debugger

Im Menü `⋮` rechts in der Reiterleiste ersetzt `Debugger` die Tastatur durch den Debugger. Er zeigt den
Code der Datei; ein Tipp auf eine Zeile setzt oder löscht dort einen Haltepunkt (roter Punkt, im
Editor rote Zeilennummer). Läuft das Programm auf einen Haltepunkt, hält es vor dieser Zeile an:
unten ist die Zeile markiert, oben stehen die Variablen – erst die lokalen der laufenden Funktion
(mit `$this`), dann die globalen, Arrays und Objekte in Kurzform.

| Knopf | Wirkung |
|---|---|
| `RUN` / `CONT` (START) | Programm starten bzw. bis zum nächsten Haltepunkt weiterlaufen lassen |
| `STEP` (A) | nächste Zeile, auch in aufgerufene Funktionen hinein; ohne laufendes Programm: starten und in der ersten Zeile anhalten; während das Programm läuft: anhalten |
| `OVER` (X) | nächste Zeile, Funktionsaufrufe laufen durch |
| `OUT` (Y) | bis zurück zum Aufrufer der Funktion |
| `STOP` | Programm abbrechen |
| `ABC` (B) | zurück zur Tastatur (die Haltepunkte bleiben) |
| ↑ ↓ | Code blättern |
| L / R, SELECT | Variablen blättern, oben zwischen Variablen und Ausgabe umschalten |

Haltepunkte gelten auch beim normalen `RUN`; der Debugger öffnet sich dann von selbst. Wartet das
Programm auf `input()`, kommt die Tastatur zurück. Code in Callbacks von `usort`, `array_map` usw.
läuft ohne Halt durch.

## Beispiele

Im Ordner [`examples/`](examples/) liegen fertige Programme, jeweils als Ordner. Zum Ausprobieren
die Ordner auf die SD-Karte nach `/nds-php` kopieren; sie erscheinen dann in der Liste auf dem
Titelbildschirm (oder `LOAD "PONG"`).

| Programm | Inhalt | Steuerung |
|---|---|---|
| [PONG](examples/PONG/) | Pong gegen den Computer, bis 7 Punkte. Hardware-Sprites für Schläger und Ball, Sounds für Treffer; `new_ball()` liegt in `ball.php` und wird per `require` eingebunden | Steuerkreuz ↑↓ oder W/S |
| [PAINT](examples/PAINT/) | Malen mit dem Stylus: `swap_screens(true)` legt die Ausgabe auf den Touchscreen, `touch()` liefert die Position | Farbe antippen oder X, Y löschen, B Ende |
| [SHAPES](examples/SHAPES/) | Zufällige Formen mit Klassen: Interface `Shape`, abstrakte Klasse `Figure` mit Trait `Colorful`, Unterklassen `Circle`, `Box`, `Square` in `shapes.php` | A neue Formen, B Ende |
| [SNAKE](examples/SNAKE/) | Snake mit Äpfeln, wird mit jedem Apfel länger und schneller. Körper per `stamp()` in der Grafik, Kopf und Apfel als Sprites, Rekord in `snake.json`, Effekte mit `sfx()` | Steuerkreuz oder W/A/S/D |

Sprites (`#SPR`) und Sounds (`#SND`) stehen jeweils in `main.php`.

## Speichern

Programme werden auf der SD-Karte (Flashcart per DLDI oder DSi-SD) unter `/nds-php` als Ordner
gespeichert, ein Ordner pro Programm:

```
SAVE "SPIEL"      speichert als /nds-php/SPIEL/main.php, util.php … (SAVE ohne Namen = letzter Name)
LOAD "SPIEL"      lädt das Programm (ersetzt das aktuelle)
DIR               listet alle Programme
DELETE "SPIEL"    löscht das Programm samt Ordner
```

Programmnamen: bis 24 Zeichen aus A–Z, 0–9, `_`, `-`. Jede Datei beginnt mit `<?php` und enthält die
Zeilen genau wie im Editor (samt Einrückung und Leerzeilen), kann also am PC bearbeitet werden.
Dateien, die nicht mehr zum Programm gehören, entfernt `SAVE` aus dem Ordner. Benutzte Sprites und
Sounds stehen am Ende von `main.php` als PHP-Kommentare: `#SPR n` + 64 Zeichen (`.` = transparent,
`0`–`F` = Farbe) und `#SND n klang tempo note:länge …` (Note als MIDI-Nummer, 0 = Pause, Länge in
16teln). Ältere Programme als einzelne Datei (`/nds-php/NAME.PHP`) lassen sich weiterhin laden.
Ohne SD-Karte meldet der Interpreter beim Start `NO SD CARD`. Programme aus älteren Versionen
(Ordner `/nds-basic`) bleiben dort unberührt; zum Weiterverwenden einfach nach `/nds-php` kopieren.

Im Emulator melonDS: unter *Config → Emu settings → DLDI* die SD-Emulation aktivieren
(optional mit „Sync SD to folder“).

## Dateien und JSON

Programme können eigene Dateien lesen und schreiben, etwa Highscores oder Spielstände. Sie liegen
im Ordner des Programms (`/nds-php/SPIEL/`). Ein noch nicht gespeichertes Programm schreibt direkt
nach `/nds-php/`. Dateinamen bestehen aus Buchstaben, Ziffern, `_`, `-` und `.` (keine Ordner,
max. 64 Zeichen); `.php`-Dateien gehören dem Editor und können nur gelesen werden.

```
file_put_contents(f, $s)    Datei schreiben, gibt die Bytes zurück oder false
                            (3. Argument FILE_APPEND: anhängen; $s darf ein Array sein)
file_get_contents(f)        Inhalt als String, false wenn nicht vorhanden
file(f, flags)              Zeilen als Array (FILE_IGNORE_NEW_LINES, FILE_SKIP_EMPTY_LINES)
file_exists(f)              true, wenn die Datei existiert
unlink(f)                   Datei löschen
json_encode($v, flags)      Wert als JSON (JSON_PRETTY_PRINT, JSON_UNESCAPED_SLASHES)
json_decode($s)             JSON lesen, null bei Fehlern; JSON-Objekte werden immer Arrays
```

```php
$best = json_decode(file_get_contents("score.json"))["best"] ?? 0;
if ($points > $best) file_put_contents("score.json", json_encode(["best" => $points]));
```

## Website

Die Landingpage liegt in [`site/`](site/), auf Englisch (`index.html`, `docs.html`) und Deutsch
(`index.de.html`, `docs.de.html`) mit Sprachumschalter im Kopf. Der Workflow
[`.github/workflows/pages.yml`](.github/workflows/pages.yml) baut bei jedem Push auf `main` die ROM im
devkitPro-Container, setzt die Seite mit `nds-php.nds`, dieser README und der englischen (als Doku-Seiten) und den
Beispielen zusammen und veröffentlicht sie über GitHub Pages.

Einmalig im Repository einstellen: *Settings → Pages → Build and deployment → Source: GitHub Actions*.

Lokal ansehen:

```bash
./build.sh && sh site/build-site.sh && python3 -m http.server -d _site 8000
```
