# 문장·선언 문법과 파서 우선순위

이 문서는 `프론트엔드/문장파서.hmje`의 `파싱한문장본문`과
`문장갈래_선언.hmje`, `문장갈래_제어.hmje`, `문장갈래_동작.hmje`에서
추출한 현재 파서의 문장·선언 계약입니다. 문법 추가 제안이 아닙니다.
표현식·타입·표면 DSL의 내부 문법은 해당 파서에 위임합니다.

이 문서의 예제는 문법 형태의 파싱을 설명하며, 타입 검사 가능한 독립 프로그램일 필요는 없습니다. 실행 가능한 예제는 `examples/`를 참고하십시오.

## 들여쓰기

블록은 공백 또는 탭 들여쓰기로 표현합니다. 탭 하나는 네 칸입니다. 한 파일 안에서 두 스타일을 섞는 것은 오류이며 같은 줄의 탭·공백 혼용도 포함합니다. 빈 줄·주석·여러 줄 문자열의 내용은 스타일 판정에서 제외합니다. 진단에는 파일:줄과 탭·공백을 처음 사용한 줄이 포함됩니다. 문법을 추가하지 않고 기존 들여쓰기 단계 규칙을 그대로 적용합니다.

## 표기와 파싱 계약

`=`는 정의, `,`는 이어쓰기, `|`는 대안, `[]`는 선택, `{}`는 반복,
`()`는 묶음, 따옴표는 터미널, `? … ?`는 다른 파서 또는 의미 술어입니다.
문자열 안의 공백은 낱말 경계를 표시합니다. 조사 터미널은 붙은 토큰과 별도
토큰을 기존 토큰화·범위 복사 규칙대로 읽습니다. `stop`은 줄 끝의 구두 토큰
자리이며, 각 함수의 정확한 토큰 개수 검사도 함께 적용합니다.

EBNF의 대안은 **아래 표의 순서대로 시도하는 대안**입니다. EBNF만으로
오류 선점이나 환경 의존 확장을 표현할 수 없으므로 다음 절차도 문법의 일부입니다.

1. `파싱줄끝`이 괄호 깊이를 세며 논리 줄 범위를 정합니다.
2. 각 갈래는 미인식이면 다음 갈래로 넘깁니다. 머리말 검사만 통과했다고
   인식한 것은 아닙니다. 내부 검사 뒤에 미인식으로 끝나는 갈래도 있습니다.
3. 인식한 성공 **또는 실패**는 즉시 확정합니다. 오류를 다음 갈래로 넘기지 않습니다.
4. `파싱한문장`이 기존 마침표·출력 조사 경고를 첫 AST 노드에 덧붙입니다.
5. `파싱블록`과 `소스문장파싱줄부터환경`이 기존 오류 동기화 규칙으로
   다음 문장까지 진행하며 최대 20개 진단을 모읍니다. 닫히지 않은 그룹은
   기존 복구 토큰화 경로를 사용합니다.

`name`, `phrase`, `expression`, `type`, `condition`은 별도 언어를 새로 정의하지
않습니다. 실제 파서의 토큰 종류, 조사 제거, 범위 소비 검사, 자연 조건 파싱을
뜻하는 외부 생산 규칙입니다. 타입 적합성·이름 해소·코드 생성 검사는 이 문서의 범위 밖입니다.

## 디스패처

같은 동사를 공유하는 문장도 앞선 갈래가 선점할 수 있습니다. 예를 들어 객체 정의는
일반 정의보다 앞서고, 메서드·모듈 실행은 대입과 일반 실행보다 앞섭니다.
기다림의 잘못된 조사는 괄호 표현식 폴백보다 먼저 진단합니다.

| 순서 | 파서 | 생산 규칙 |
| --- | --- | --- |
| 1 | `파싱문장기다림조사오류` | `wait-particle-error` |
| 2 | `파싱문장현재창닫기` | `close-window` |
| 3 | `파싱문장현재창앞으로` | `front-window` |
| 4 | `파싱문장새창열기` | `open-window` |
| 5 | `파싱문장앱사건위치오류` | `app-event-error` |
| 6 | `파싱문장열린주소오류` | `open-address-error` |
| 7 | `UI표면문장파싱` | `ui-statement` |
| 8 | `DB표면문장파싱` | `db-statement` |
| 9 | `라우트표면문장파싱` | `route-statement` |
| 10 | `채널표면문장파싱` | `channel-statement` |
| 11 | `배열파이프라인표면문장파싱` | `pipeline-statement` |
| 12 | `파싱문장클래스` | `class-declaration` |
| 13 | `파싱문장확장` | `extension-declaration` |
| 14 | `파싱문장타입필드수식어` | `type-field-modifier` |
| 15 | `파싱문장옛수식어오류` | `old-modifier-error` |
| 16 | `파싱문장제네릭` | `generic-declaration` |
| 17 | `파싱문장반복계속` | `continue-statement` |
| 18 | `파싱문장다음반복` | `next-statement` |
| 19 | `파싱문장반복종료` | `break-statement` |
| 20 | `파싱문장빈반환` | `empty-return` |
| 21 | `파싱문장블록수식어` | `block-modifier` |
| 22 | `파싱문장함수수식어` | `function-modifier` |
| 23 | `파싱문장저장수식어` | `storage-modifier` |
| 24 | `파싱문장반복수식어` | `iteration-modifier` |
| 25 | `파싱문장파일가져오기` | `file-import` |
| 26 | `파싱문장모듈가져오기` | `module-import` |
| 27 | `파싱문장모듈전체내보내기` | `module-export` |
| 28 | `파싱문장모듈선택내보내기` | `selective-export` |
| 29 | `파싱문장접근상수` | `visible-constant` |
| 30 | `파싱문장외부전역` | `external-global` |
| 31 | `파싱문장전역정의` | `global-definition` |
| 32 | `파싱문장위험블록` | `unsafe-block` |
| 33 | `파싱문장선점금지블록` | `preempt-block` |
| 34 | `파싱문장동시블록` | `concurrent-block` |
| 35 | `파싱문장비동기블록` | `async-block` |
| 36 | `파싱문장제한시간블록` | `deadline-block` |
| 37 | `파싱문장시도블록` | `try-block` |
| 38 | `파싱문장특성` | `trait-declaration` |
| 39 | `파싱문장열거` | `enum-declaration` |
| 40 | `파싱문장실패` | `throw-statement` |
| 41 | `파싱문장외부함수` | `external-function` |
| 42 | `파싱문장함수` | `function-declaration` |
| 43 | `파싱문장구조체` | `struct-declaration` |
| 44 | `파싱문장각반복` | `each-statement` |
| 45 | `파싱문장함수값` | `function-value` |
| 46 | `파싱문장조건` | `if-statement` |
| 47 | `파싱문장경우` | `case-statement` |
| 48 | `파싱문장동안` | `while-statement` |
| 49 | `파싱문장반환` | `return-statement` |
| 50 | `파싱문장한항목빌림` | `simple-borrow` |
| 51 | `파싱문장범위빌림` | `expression-borrow` |
| 52 | `파싱문장참조` | `reference-definition` |
| 53 | `파싱문장정의옮김` | `move-definition` |
| 54 | `파싱문장대상옮김` | `move-target` |
| 55 | `파싱문장주소고정` | `pin-address` |
| 56 | `파싱문장해제` | `release-statement` |
| 57 | `파싱문장메모리해제` | `free-memory` |
| 58 | `파싱문장메모리복사` | `copy-memory` |
| 59 | `파싱문장잊기` | `forget-statement` |
| 60 | `파싱문장입력` | `input-statement` |
| 61 | `파싱문장메모리할당` | `allocate-memory` |
| 62 | `파싱문장객체정의` | `object-definition` |
| 63 | `파싱문장객체사용종료` | `finish-object` |
| 64 | `파싱문장객체쓰기` | `write-object` |
| 65 | `파싱문장객체연결` | `link-object` |
| 66 | `파싱문장객체짝연결` | `link-pair` |
| 67 | `파싱문장객체임시해제` | `release-temporary` |
| 68 | `파싱문장객체저장` | `save-object` |
| 69 | `파싱문장소유권넘김` | `transfer-ownership` |
| 70 | `파싱문장정의` | `local-definition` |
| 71 | `파싱문장문자열붙이기` | `append-string` |
| 72 | `파싱문장모음추가` | `append-collection` |
| 73 | `파싱문장번째제거` | `remove-ordinal` |
| 74 | `파싱문장색인제거` | `remove-index` |
| 75 | `파싱문장번째삽입` | `insert-ordinal` |
| 76 | `파싱문장색인삽입` | `insert-index` |
| 77 | `파싱문장모음확보` | `reserve-collection` |
| 78 | `파싱문장모음비우기` | `clear-collection` |
| 79 | `파싱문장사전대입` | `dictionary-assignment` |
| 80 | `파싱문장사전지우기` | `delete-dictionary` |
| 81 | `파싱문장관계등록` | `register-relation` |
| 82 | `파싱문장빈실행` | `empty-execute` |
| 83 | `파싱문장점실행` | `dot-execute` |
| 84 | `파싱문장모듈실행` | `module-execute` |
| 85 | `파싱문장메서드실행` | `method-execute` |
| 86 | `파싱문장대입` | `assignment` |
| 87 | `파싱문장기다림` | `wait-statement` |
| 88 | `파싱문장코드종료` | `exit-statement` |
| 89 | `파싱문장인자실행` | `argument-execute` |
| 90 | `파싱문장출력` | `print-statement` |
| 91 | `파싱문장표현식` | `expression-statement` |

## EBNF

블록 머리에는 마침표를 덧붙이지 않습니다. `newline`, `indent`, `dedent`는
토큰화기가 만든 줄바꿈·들여쓰기·내어쓰기 토큰입니다. 빈 줄은 블록과 파일 루프가 건너뜁니다.

```ebnf
source = { newline | statement } ;
statement = declaration | control | operation | surface-statement | expression-statement ;
declaration = class-declaration | extension-declaration | type-field-modifier
            | generic-declaration | block-modifier | function-modifier | storage-modifier
            | iteration-modifier | file-import | module-import | module-export
            | selective-export | visible-constant | external-global | global-definition
            | trait-declaration | enum-declaration | external-function | function-declaration
            | struct-declaration | function-value | local-definition ;
control = continue-statement | next-statement | break-statement | empty-return
        | unsafe-block | preempt-block | concurrent-block | async-block | deadline-block
        | try-block | each-statement | if-statement | case-statement | while-statement
        | return-statement | throw-statement ;
operation = close-window | front-window | open-window | simple-borrow | expression-borrow
          | reference-definition | move-definition | move-target | pin-address
          | release-statement | free-memory | copy-memory | forget-statement | input-statement
          | allocate-memory | object-definition | finish-object | write-object | link-object
          | link-pair | release-temporary | save-object | transfer-ownership | append-string
          | append-collection | remove-ordinal | remove-index | insert-ordinal | insert-index
          | reserve-collection | clear-collection | dictionary-assignment | delete-dictionary
          | register-relation | empty-execute | dot-execute | module-execute | method-execute
          | assignment | wait-statement | exit-statement | argument-execute | print-statement ;
surface-statement = ui-statement | db-statement | route-statement | channel-statement | pipeline-statement ;
block = newline, indent, { newline | statement }, dedent ;
stop = punctuation, newline ;
punctuation = "." | "," ;
newline = ? 줄바꿈 토큰 또는 소스 끝 ? ;
indent = ? 들여쓰기 토큰 ? ;
dedent = ? 내어쓰기 토큰 ? ;
name = ? 이름 토큰; 각 선언의 조사 제거 규칙 적용 ? ;
phrase = ? 파싱토큰범위본문으로 합치는 하나 이상의 토큰 ? ;
string = ? 문자열 토큰; JSON 문자열·보간 파서에 위임 ? ;
integer = ? 정수 토큰 ? ;
expression = ? 파싱표현식범위 또는 해당 갈래의 정확/그대로 범위 파서 ? ;
type = ? 기존 타입 표기; 파싱AST노드의 숫자 타입 정규화 적용 ? ;
condition = ? 파싱자연조건 또는 파싱동안조건 ? ;
pattern = ? 기존 값 패턴; payload 패턴은 타입 검사에서 검증 ? ;
assignment-target = ? 파싱대상변환; 이름·필드·색인·오프셋 메모리 대상 ? ;
dictionary-access = ? 표현식사전조회시작인가 및 파싱사전조회식 ? ;
subject-particle = "이" | "가" ;
object-particle = "을" | "를" ;
topic-particle = "은" | "는" ;
visibility = "공개" | ("프로젝트" | "모듈" | "파일"), "내부" ;
access-word = "공개" | "프로젝트" | "모듈" | "파일" | "내부" ;
visibility-prefix = access-word, { access-word } ;
mutability = "변수" | "상수" ;
function-value-return = "반환하지 않음" | "비동기로 반환하지 않음"
                      | type, object-particle, [ "비동기로" ], "반환"
                      | ? 반환으로 끝나지 않는 기존 타입 표기 ? ;
function-modifier-word = ? 파싱함수수식자인가 및 일반 함수 머리 검사 ? ;
class-modifiers = ? 파싱클래스머리점 앞의 기존 클래스 수식어 ? ;
return-prefix = type, object-particle, "반환하는" ;
error-prefix = type, "로 실패할 수 있는" ;
parameters = "(", [ parameter, { ",", parameter } ], ")" ;
parameter = name, ":", type, [ "=", expression ] ;
arguments = "(", (named-arguments | positional-arguments), ")" ;
named-arguments = name, ":", expression, { ",", name, ":", expression } ;
positional-arguments = expression, { ",", expression } ;
execute-verb = "실행한다" | "시도한다" ;
quantity = integer | "(", expression, ")" ;
unit = "밀리초" | "초" | "분" | "시간" ;
definition-tail = (expression, "이고", ",", type, "이다" | type, "이다" | expression), stop ;
section = "섹션", string, "에" ;
alignment = integer, "로 정렬한" ;
export-name = name, [ "와" | "과" ] ;
class-body = ? 파싱클래스선언의 필드·초기화·함수 블록 ? ;
modifier-properties = ? 파싱수식어선언의 속성 블록 ? ;
modifier-hooks = ? 파싱함수수식어선언의 기존 훅 블록 ? ;
storage-hooks = ? 파싱저장수식어선언의 시작·읽기·쓰기·끝 훅 블록 ? ;
iteration-hooks = newline, indent, [ "시작할 때", block ], [ "매 반복에", block ], [ "끝날 때", block ], dedent ;
trait-body = newline, indent, trait-item, { newline | trait-item }, dedent ;
trait-item = phrase, "만족해야 하고", [ punctuation ], newline
           | [ phrase, object-particle, "만족하는" ], "임의의 타입", name, [ punctuation ], newline
           | parameters, object-particle, "받아", type, object-particle, "반환하는 함수", name, [ "이다" ], stop ;
struct-body = newline, indent, { struct-field-line }, { initializer }, { function-declaration }, dedent ;
struct-field-line = struct-field, { ",", struct-field }, punctuation, newline ;
struct-field = [ field-visibility ], [ phrase ], [ mutability ], name, topic-particle, type, ("이고" | "이다") ;
field-visibility = visibility | "타입 내부" ;
initializer = ? 기존 파싱초기화선언의 초기화 머리와 본문; 초기화파서.hmje ? ;
each-tail = ? 파싱각반복머리종류; 마다·에 대해·수식어 반복, 환경 의존 전개 ? ;
enum-body = newline, indent, (sentence-cases | legacy-cases), dedent ;
sentence-cases = enum-case, { ("와" | "과"), ",", newline, enum-case }, "이다", ".", newline ;
legacy-cases = enum-case, newline, { enum-case, newline } ;
enum-case = phrase, [ "=", expression ] | name, parameters ;
```

문장형 열거는 중간 항목을 `와/과,`, 마지막을 `이다.`로 닫습니다. 블록형 열거와
섞지 않습니다. payload 항목은 원시값을 갖지 않습니다. 이 검사는 `파싱문장열거`에 있습니다.
경우·함수의 새 표기는 이미 main에 있던 구현을 그대로 기록합니다.

### 1. `wait-particle-error`

```ebnf
wait-particle-error = quantity, unit, object-particle, "기다린다", stop ;
```

이 생산 규칙은 성공 문법이 아닌 기존 선행 오류 검사입니다.

```hmje
3초를 기다린다.
```

### 2. `close-window`

```ebnf
close-window = "이 창을 닫는다", stop ;
```

```hmje
이 창을 닫는다.
```

### 3. `front-window`

```ebnf
front-window = "이 창을 앞으로 가져온다", stop ;
```

```hmje
이 창을 앞으로 가져온다.
```

### 4. `open-window`

```ebnf
open-window = expression, object-particle, "새 창으로 연다", stop ;
```

```hmje
화면값을 새 창으로 연다.
```

### 5. `app-event-error`

```ebnf
app-event-error = ("URL이 열리면" | "앱이 시작되면" | "앱이 다시 열리면" | "마지막 창이 닫히면"), block ;
```

이 생산 규칙은 성공 문법이 아닌 기존 선행 오류 검사입니다.

```hmje
URL이 열리면
    반환한다.
```

### 6. `open-address-error`

```ebnf
open-address-error = ? 파싱열린주소점: 열린 주소 뒤의 폐기 구문 ? ;
```

이 생산 규칙은 성공 문법이 아닌 기존 선행 오류 검사입니다.

```hmje
주소를 열린 주소로 변환한다.
```

### 7. `ui-statement`

```ebnf
ui-statement = ? UI표면문장파싱; docs/language.md ? ;
```

```hmje
화면 예시화면의
    추적 대상 숫자는 0이고, 자연수이다.
    그리는 내용은
        수직배치의
            간격은 8이고,
            내용은
                텍스트의
                    내용은 "안녕"이다.
```

### 8. `db-statement`

```ebnf
db-statement = ? DB표면문장파싱; docs/language.md ? ;
```

```hmje
SQLiteAsync를 가져온다.
저장되는 클래스 사용자의
    저장 이름은 "users"이다.

    상수 필드 식별자의
        타입은 자연수이고,
        저장 이름은 "id"이고,
        기본키이다.
    변수 필드 이름의
        타입은 문자열이고,
        저장 이름은 "name"이다.

직접 질의 사용자찾기의
    사용자에서 사용자를 선택한다.
    사용자.식별자 == 식별자를 걸러낸다.

비동기인 함수 앱시작은
    상수 열기는 SQLite비동기열기(":memory:")을 기다린 결과이고, SQLite비동기열기결과이다.
    상수 데이터베이스는 열기.연결이고, SQLite비동기연결이다.
    상수 생성은 SQLite비동기실행(데이터베이스, "CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT NOT NULL)")을 기다린 결과이고, 정수32이다.
    사용자들에 추가할 행의
        식별자는 자연수(3)이고,
        이름은 "홍길동"이다.
    상수 사람들은 사용자찾기(식별자: 3)을 기다린 결과이다.
    만약 사람들이 없으면
        출력을 (문구: "질의 실패")로 실행한다.
        반환한다.
    출력을 (값: 사람들!.길이)로 실행한다.
    출력을 (문구: 사람들![0].이름)로 실행한다.
    상수 닫기는 SQLite비동기닫기(데이터베이스)을 기다린 결과이고, 정수32이다.
```

### 9. `route-statement`

```ebnf
route-statement = ? 라우트표면문장파싱; docs/language.md ? ;
```

```hmje
라우트 묶음 API는
    GET 요청 "/사용자/{식별자}"는 (식별자: 자연수)를 받아
        식별자를 자연수로 반환한다.

서버의 라우트들은
    GET 요청 "/상태"는
        200을 자연수로 반환한다.
```

### 10. `channel-statement`

```ebnf
channel-statement = ? 채널표면문장파싱; docs/language.md ? ;
```

```hmje
1을 통로로 보낸다.
```

### 11. `pipeline-statement`

```ebnf
pipeline-statement = ? 배열파이프라인표면문장파싱; docs/language.md ? ;
```

```hmje
상수 결과는 목록들의 각 값을 값 + 1로 바꾼 것이다.
```

### 12. `class-declaration`

```ebnf
class-declaration = class-modifiers, "클래스", name, "의", class-body ;
```

```hmje
클래스 예시의
    값은 자연수이다.
```

### 13. `extension-declaration`

```ebnf
extension-declaration = "확장된", name, "의", newline, indent, function-declaration, { function-declaration }, dedent ;
```

```hmje
확장된 예시의
    함수 동작은
        반환한다.
```

### 14. `type-field-modifier`

```ebnf
type-field-modifier = [ "공개" ], ("타입" | "필드"), "수식어", phrase, "의", modifier-properties ;
```

```hmje
타입 수식어 예시의
    활용은 하다이고,
    붙는 곳은 화면이다.
```

### 15. `old-modifier-error`

```ebnf
old-modifier-error = [ "공개" ], ("타입" | "필드"), "수식어", phrase, topic-particle, block ;
```

이 생산 규칙은 성공 문법이 아닌 기존 선행 오류 검사입니다.

```hmje
타입 수식어 예시는
    반환한다.
```

### 16. `generic-declaration`

```ebnf
generic-declaration = [ phrase, object-particle, "만족하는" ], "임의의 타입", name, "에 대해", [ "," ], newline, declaration ;
```

```hmje
임의의 타입 T에 대해
함수 동작은
    반환한다.
```

### 17. `continue-statement`

```ebnf
continue-statement = "반복을 계속한다", stop ;
```

```hmje
반복을 계속한다.
```

### 18. `next-statement`

```ebnf
next-statement = "다음 반복으로 넘어간다", stop ;
```

```hmje
다음 반복으로 넘어간다.
```

### 19. `break-statement`

```ebnf
break-statement = "반복을 종료한다", stop ;
```

```hmje
반복을 종료한다.
```

### 20. `empty-return`

```ebnf
empty-return = "반환한다", stop ;
```

```hmje
반환한다.
```

### 21. `block-modifier`

```ebnf
block-modifier = "블록 수식어", phrase, topic-particle, [ parameters, object-particle, "받아" ], block ;
```

```hmje
블록 수식어 예시는 (내용: 자연수의 배열)를 받아
    내용을 자연수의 배열로 반환한다.
```

### 22. `function-modifier`

```ebnf
function-modifier = "함수 수식어", phrase, topic-particle, [ parameters, object-particle, "받아" ], modifier-hooks ;
```

```hmje
함수 수식어 예시는
    시작할 때
        반환한다.
```

### 23. `storage-modifier`

```ebnf
storage-modifier = "저장 수식어", phrase, topic-particle, [ parameters, object-particle, "받아" ], storage-hooks ;
```

```hmje
저장 수식어 예시는 (값: 자연수)를 받아
    읽을 때
        값을 자연수로 반환한다.
```

### 24. `iteration-modifier`

```ebnf
iteration-modifier = "반복 수식어", phrase, topic-particle, [ parameters, object-particle, "받아" ], iteration-hooks ;
```

```hmje
반복 수식어 예시는
    시작할 때
        반환한다.
```

### 25. `file-import`

```ebnf
file-import = string, object-particle, "가져온다", stop ;
```

```hmje
"동작.hmje"를 가져온다.
```

### 26. `module-import`

```ebnf
module-import = phrase, object-particle, "가져온다", stop ;
```

```hmje
기반을 가져온다.
```

### 27. `module-export`

```ebnf
module-export = phrase, object-particle, "가져와 내보낸다", stop ;
```

```hmje
기반을 가져와 내보낸다.
```

### 28. `selective-export`

```ebnf
selective-export = name, "의", export-name, { ",", export-name }, object-particle, "내보낸다", stop ;
```

```hmje
기반의 하나와, 둘을 내보낸다.
```

### 29. `visible-constant`

```ebnf
visible-constant = visibility-prefix, "상수", name, topic-particle, definition-tail ;
```

```hmje
공개 상수 값은 1이고, 자연수이다.
```

### 30. `external-global`

```ebnf
external-global = "외부 전역 심볼", name, topic-particle, type, "이다", stop ;
```

```hmje
외부 전역 심볼 값은 자연수이다.
```

### 31. `global-definition`

```ebnf
global-definition = { access-word }, "전역", mutability, name, topic-particle, [ section ], [ alignment ], definition-tail ;
```

```hmje
공개 전역 변수 값은 1이고, 자연수이다.
```

### 32. `unsafe-block`

```ebnf
unsafe-block = "위험 작업은", block ;
```

```hmje
위험 작업은
    반환한다.
```

### 33. `preempt-block`

```ebnf
preempt-block = "선점금지 작업은", block ;
```

```hmje
선점금지 작업은
    반환한다.
```

### 34. `concurrent-block`

```ebnf
concurrent-block = "동시 작업은", block ;
```

```hmje
동시 작업은
    반환한다.
```

### 35. `async-block`

```ebnf
async-block = "비동기 작업은", block ;
```

```hmje
비동기 작업은
    반환한다.
```

### 36. `deadline-block`

```ebnf
deadline-block = "제한 시간이", expression, "밀리초인 작업은", block ;
```

```hmje
제한 시간이 3밀리초인 작업은
    반환한다.
```

### 37. `try-block`

```ebnf
try-block = "시도할 작업은", block, [ "실패하면", block ], [ "끝으로", block ] ;
```

```hmje
시도할 작업은
    반환한다.
실패하면
    반환한다.
끝으로
    반환한다.
```

### 38. `trait-declaration`

```ebnf
trait-declaration = { access-word }, "특성", phrase, object-particle, "만족하는", name, topic-particle, trait-body ;
```

```hmje
특성 예시를 만족하는 T는
    (값: 자연수)를 받아 자연수를 반환하는 함수 동작이다.
```

### 39. `enum-declaration`

```ebnf
enum-declaration = [ type ], name, "의", [ visibility ], "종류는", enum-body ;
```

```hmje
문자열 예시의 공개 종류는
    하나 = "one"와,
    둘 = "two"이다.
```

### 40. `throw-statement`

```ebnf
throw-statement = expression, ("로" | "으로"), "실패한다", stop ;
```

```hmje
오류값으로 실패한다.
```

### 41. `external-function`

```ebnf
external-function = { function-modifier-word | "외부" }, [ return-prefix ], "함수", name, topic-particle, [ parameters, object-particle, "받아" ], [ type, ("반환" | "반환하고" | "반환한다") ], "외부에서", [ string ], ? 나머지 구현 문구 ? , stop ;
```

```hmje
외부 함수 동작은 (값: 자연수)를 받아 자연수를 반환하고 외부에서 "action"으로 구현된다.
```

### 42. `function-declaration`

```ebnf
function-declaration = [ error-prefix ], { function-modifier-word }, [ return-prefix ], "함수", name, topic-particle, [ parameters, object-particle, "받아" ], block ;
```

```hmje
예시오류로 실패할 수 있는 함수 동작은 (값: 자연수)를 받아
    값을 자연수로 반환한다.
```

### 43. `struct-declaration`

```ebnf
struct-declaration = { access-word }, [ "리틀엔디안" | "빅엔디안" ], [ "밀착된" ], "구조체", name, "의", struct-body ;
```

```hmje
공개 구조체 예시의
    공개 값은 자연수이고,
    이름은 문자열이다.
```

### 44. `each-statement`

```ebnf
each-statement = expression, ("에서" | "의"), "각", name, each-tail, block ;
```

```hmje
목록에서 각 값마다
    값을 출력한다.
```

### 45. `function-value`

```ebnf
function-value = ("상수" | "변수"), name, topic-particle, parameters, object-particle, "받아", [ function-value-return ], block ;
```

```hmje
상수 동작은 (값: 자연수)를 받아 자연수를 반환
    값을 자연수로 반환한다.
```

### 46. `if-statement`

```ebnf
if-statement = "만약", condition, block, { "그렇지 않고", condition, block }, [ "아니면", block ] ;
```

```hmje
만약 값 == 1이면
    반환한다.
그렇지 않고 값 == 2이면
    반환한다.
아니면
    반환한다.
```

### 47. `case-statement`

```ebnf
case-statement = expression, subject-particle, pattern, "인 경우", block, { pattern, "인 경우", block }, [ "그 밖의 경우", block ] ;
```

```hmje
값이 하나인 경우
    반환한다.
둘인 경우
    반환한다.
그 밖의 경우
    반환한다.
```

### 48. `while-statement`

```ebnf
while-statement = condition, "동안", block ;
```

```hmje
값 < 3일 동안
    반복을 종료한다.
```

### 49. `return-statement`

```ebnf
return-statement = expression, object-particle, [ type, ("로" | "으로") ], "반환한다", stop ;
```

```hmje
값을 자연수로 반환한다.
```

### 50. `simple-borrow`

```ebnf
simple-borrow = "변수", name, topic-particle, name, ("에서" | object-particle), type, object-particle, ("빌려온다" | "가져온다"), stop ;
```

```hmje
변수 값은 주소에서 자연수를 빌려온다.
```

### 51. `expression-borrow`

```ebnf
expression-borrow = "변수", name, topic-particle, expression, ("에서" | object-particle), type, object-particle, ("빌려온다" | "가져온다"), stop ;
```

```hmje
변수 값은 주소들[0]에서 자연수를 빌려온다.
```

### 52. `reference-definition`

```ebnf
reference-definition = "상수", name, topic-particle, expression, object-particle, "참조한다", { punctuation } ;
```

```hmje
상수 값은 원본.필드를 참조한다.
```

### 53. `move-definition`

```ebnf
move-definition = "변수", name, topic-particle, name, object-particle, "옮긴다", { punctuation } ;
```

```hmje
변수 값은 원본을 옮긴다.
```

### 54. `move-target`

```ebnf
move-target = name, object-particle, name, ("로" | "으로"), "옮긴다", { punctuation } ;
```

```hmje
원본을 값으로 옮긴다.
```

### 55. `pin-address`

```ebnf
pin-address = name, "의 주소를 고정한다", { punctuation } ;
```

```hmje
값의 주소를 고정한다.
```

### 56. `release-statement`

```ebnf
release-statement = name, object-particle, "해제한다", stop ;
```

```hmje
값을 해제한다.
```

### 57. `free-memory`

```ebnf
free-memory = expression, "의 메모리를 해제한다", stop ;
```

```hmje
주소의 메모리를 해제한다.
```

### 58. `copy-memory`

```ebnf
copy-memory = expression, "에서", expression, "로", expression, "바이트 메모리를 복사한다", stop ;
```

```hmje
원본에서 대상로 8바이트 메모리를 복사한다.
```

### 59. `forget-statement`

```ebnf
forget-statement = name, object-particle, "잊는다", stop ;
```

```hmje
값을 잊는다.
```

### 60. `input-statement`

```ebnf
input-statement = [ "상수" | "변수" ], name, object-particle, type, ("로" | "으로"), "입력받는다", stop ;
```

```hmje
변수 값을 문자열로 입력받는다.
```

### 61. `allocate-memory`

```ebnf
allocate-memory = ("상수" | "변수"), name, topic-particle, expression, "바이트 메모리를 할당한다", stop ;
```

```hmje
변수 주소는 8바이트 메모리를 할당한다.
```

### 62. `object-definition`

```ebnf
object-definition = [ "상수" | "변수" ], name, topic-particle, (expression, "의 객체이다" | expression, object-particle, ("문자열로" | "문자열으로"), "읽는다"), stop ;
```

```hmje
상수 문서는 "/예시"의 객체이다.
```

### 63. `finish-object`

```ebnf
finish-object = expression, ("사용을" | "사용를"), "끝낸다", stop ;
```

```hmje
문서 사용을 끝낸다.
```

### 64. `write-object`

```ebnf
write-object = (expression, "에", ("문자열로" | "문자열으로"), expression, object-particle | expression, object-particle, expression, "에"), "쓴다", stop ;
```

```hmje
문서에 문자열로 "내용"을 쓴다.
```

### 65. `link-object`

```ebnf
link-object = expression, object-particle, expression, "에", expression, ("로" | "으로"), [ "강하게" ], "연결한다", stop ;
```

```hmje
문서를 원본에 관계로 강하게 연결한다.
```

### 66. `link-pair`

```ebnf
link-pair = expression, ("와" | "과"), expression, object-particle, expression, ("로" | "으로"), [ "강하게" ], "연결한다", stop ;
```

```hmje
왼쪽과 오른쪽을 관계로 연결한다.
```

### 67. `release-temporary`

```ebnf
release-temporary = expression, "의 임시 상태를 해제한다", stop ;
```

```hmje
문서의 임시 상태를 해제한다.
```

### 68. `save-object`

```ebnf
save-object = expression, [ object-particle ], "저장한다", stop ;
```

```hmje
문서를 저장한다.
```

### 69. `transfer-ownership`

```ebnf
transfer-ownership = name, "의 소유권을", phrase, ("에" | "에게"), "넘긴다", stop ;
```

```hmje
원본의 소유권을 대상에 넘긴다.
```

### 70. `local-definition`

```ebnf
local-definition = ("단" | "자" | "상수" | "변수"), name, topic-particle, definition-tail ;
```

```hmje
변수 값은 1이고, 자연수이다.
```

### 71. `append-string`

```ebnf
append-string = name, "에", expression, object-particle, "붙인다", stop ;
```

```hmje
글에 "내용"을 붙인다.
```

### 72. `append-collection`

```ebnf
append-collection = name, "에", expression, object-particle, ("추가한다" | "넣는다"), stop ;
```

```hmje
목록에 값을 추가한다.
```

### 73. `remove-ordinal`

```ebnf
remove-ordinal = name, "에서", expression, "번째 값을 제거한다", stop ;
```

```hmje
목록에서 0번째 값을 제거한다.
```

### 74. `remove-index`

```ebnf
remove-index = expression, "[", expression, "]", object-particle, "제거한다", stop ;
```

```hmje
목록[0]을 제거한다.
```

### 75. `insert-ordinal`

```ebnf
insert-ordinal = name, "에서", expression, "번째 위치에", expression, object-particle, "삽입한다", stop ;
```

```hmje
목록에서 0번째 위치에 값을 삽입한다.
```

### 76. `insert-index`

```ebnf
insert-index = expression, "[", expression, "]", "에", expression, object-particle, "삽입한다", stop ;
```

```hmje
목록[0]에 값을 삽입한다.
```

### 77. `reserve-collection`

```ebnf
reserve-collection = expression, subject-particle, expression, "개를 담을 메모리를 확보한다", stop ;
```

```hmje
목록이 8개를 담을 메모리를 확보한다.
```

### 78. `clear-collection`

```ebnf
clear-collection = expression, object-particle, "비운다", stop ;
```

```hmje
목록을 비운다.
```

### 79. `dictionary-assignment`

```ebnf
dictionary-assignment = name, "에서", expression, "의 값은", expression, [ "이다" ], stop ;
```

```hmje
사전에서 "열쇠"의 값은 1이다.
```

### 80. `delete-dictionary`

```ebnf
delete-dictionary = name, "에서", expression, object-particle, "지운다", stop ;
```

```hmje
사전에서 "열쇠"를 지운다.
```

### 81. `register-relation`

```ebnf
register-relation = expression, "관계를", expression, ("설명으로" | "설명로"), "등록한다", stop ;
```

```hmje
"관계" 관계를 "설명" 설명으로 등록한다.
```

### 82. `empty-execute`

```ebnf
empty-execute = name, object-particle, execute-verb, stop ;
```

```hmje
동작을 실행한다.
```

### 83. `dot-execute`

```ebnf
dot-execute = expression, ".", name, [ arguments, ("로" | "으로") ], execute-verb, stop ;
```

```hmje
대상.동작을 실행한다.
```

### 84. `module-execute`

```ebnf
module-execute = name, "의", name, object-particle, [ arguments, ("로" | "으로") ], execute-verb, stop ;
```

```hmje
기반의 동작을 실행한다.
```

### 85. `method-execute`

```ebnf
method-execute = expression, "의", name, object-particle, [ arguments, ("로" | "으로") ], execute-verb, stop ;
```

```hmje
묶음.프로젝트의 동작을 실행한다.
```

### 86. `assignment`

```ebnf
assignment = assignment-target, topic-particle, expression, [ "이다" ], stop ;
```

```hmje
목록[0]은 1이다.
```

### 87. `wait-statement`

```ebnf
wait-statement = quantity, unit, "기다린다", stop ;
```

```hmje
(간격)밀리초 기다린다.
```

### 88. `exit-statement`

```ebnf
exit-statement = "코드", expression, "로 종료한다", stop ;
```

```hmje
코드 0로 종료한다.
```

### 89. `argument-execute`

```ebnf
argument-execute = name, object-particle, arguments, ("로" | "으로"), execute-verb, stop ;
```

```hmje
동작을 (값: 1)로 실행한다.
```

### 90. `print-statement`

```ebnf
print-statement = expression, [ object-particle ], [ "오류로" | "이어" | "줄바꿈 없이" ], "출력한다", stop ;
```

```hmje
"값은 \(값)"을 오류로 출력한다.
```

### 91. `expression-statement`

```ebnf
expression-statement = expression, [ punctuation ] ;
```

```hmje
동작(1).
```

## 호환성 조건과 검증

이 EBNF는 문장·선언의 구조와 디스패처의 진입 계약을 나타냅니다. 구현은 일부
기존 구문에서 마지막 구두 토큰의 본문보다 토큰 개수·동사 위치를 검사하고,
범위 파서가 조사·`이다`·구두점을 제거하기도 합니다. EBNF를 새로운 엄격한
검증기로 사용하면 이러한 기존 허용 범위와 진단이 달라질 수 있습니다.
문법의 외부 생산 규칙과 각 함수의 가드·범위 소비 검사를 함께 적용해야 합니다.

