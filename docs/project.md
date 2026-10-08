# HMJE 프로젝트·모듈 명세 v0

이 문서는 `Project.hmjep`, `Project.hmjelock`, 모듈 가져오기, 산출물과 접근
수준의 정본 명세입니다. 기존 `.hmje` 경로 가져오기는 단일 파일 모드와 마이그레이션
호환을 위해 유지합니다.

## 1. 단위

- **프로젝트**: 이름·버전·외부 의존성을 가지는 해석 및 배포 단위입니다.
- **모듈**: 소스 발견·이름 해석·접근 제어 단위입니다.
- **산출물**: 앱, 실행 파일, 라이브러리 또는 시험입니다.
- **외부 구현**: HMJE 모듈의 외부 함수를 구현하는 C 계열 원본과 링크 의존성을
  묶는, 가져올 수 없는 빌드 단위입니다.
- **빌드 목적지**: 실제 플랫폼, simulator/device, architecture와 configuration의
  조합입니다. 프로젝트 선언이 지원 범위를 정하고 빌드 명령이 목적지를 고릅니다.

프로젝트 자체에는 앱/라이브러리 종류가 없습니다. 한 프로젝트가 여러 모듈과 산출물을
가질 수 있습니다. v0 라이브러리는 소비 프로젝트와 함께 컴파일되는 **소스
라이브러리**입니다. 사전 컴파일 framework/archive와 ABI 안정성은 이 명세 밖입니다.

## 2. 프로젝트 파일

- 사람이 작성하는 파일 이름은 `Project.hmjep`입니다.
- 도구가 생성하는 잠금 파일 이름은 `Project.hmjelock`입니다.
- 두 파일 모두 버전 관리에 커밋하는 것을 권장합니다.
- `Project.hmjep`는 정적 선언 파일이며 임의 코드를 실행하지 않습니다.
- `Project.hmjelock`은 정확한 버전, Git revision과 무결성을 고정합니다.

### 2.1 선언 블록

선언 블록은 `<선언 종류> <이름>의`로 엽니다. 하위 선언은 소유 선언을 주어에
포함하고 다시 `의`로 엽니다.

```hmjep
프로젝트 NoteApp의
    버전은 "0.1.0"이다.

NoteApp 앱의 iOS 설정의
    묶음 식별자는 "com.example.notes"이다.
```

`의`로 연 블록은 비어 있을 수 없습니다. 속성 중간 문장은 `이고,`, 마지막 문장은
`이다.`로 끝납니다.

```hmjep
모듈 NotesApp의
    원본 경로는 "Sources/NotesApp"이고,
    자원 경로는 "Resources/NoteApp"이다.
```

### 2.2 목록 속성

반복되는 값은 속성을 반복하지 않고 목록 블록 하나로 씁니다. 중간 항목은 `과,`
또는 `와,`, 마지막 항목은 `이다.`로 끝납니다. 한 항목이면 같은 줄에 쓸 수 있습니다.

```hmjep
모듈 NotesApp의
    원본 경로는 "Sources/NotesApp"이고,
    의존 모듈은 NotesCore이고,
    의존 라이브러리는 HmjeUI이다.

라이브러리 HmjeUI의
    내보내는 모듈은 HmjeUI이다.
```

### 2.2.1 해석 오류

해석할 수 없는 줄은 무시하지 않고 빌드 오류입니다. 오류는 `<경로>:<줄>: <내용>`으로
알립니다(줄은 1부터).

- 선언 머리(`<종류> <이름>의`, `<산출물> <종류>의 <플랫폼> 설정의`)가 아닌 최상위 줄.
- 속성·플랫폼 설정 머리·목록 값 어느 것도 아닌 들여 쓴 줄.
- `과,`·`와,`·`이고,`·`이다.`로 끝나지 않는 목록 값 줄.
- 남은 git 충돌 표시(`<<<<<<<`, `=======`, `>>>>>>>`, `|||||||`로 시작하는 줄).
- 한 선언 블록(같은 플랫폼 설정 안)에서 같은 속성을 두 번 여는 것. 여러 값은 §2.2
  목록 하나로 씁니다. 서로 다른 플랫폼 설정의 같은 속성은 괜찮습니다.

### 2.3 전체 예시

```hmjep
프로젝트 NoteApp의
    버전은 "0.1.0"이다.

의존 프로젝트 hmje-ui의
    깃 주소는 "https://public.example.invalid/hmje/hmje-ui.git"이고,
    요구 버전은 "1.0.0 이상 2.0.0 미만"이다.

모듈 NotesCore의
    원본 경로는 "Sources/NotesCore"이다.

모듈 NotesApp의
    원본 경로는 "Sources/NotesApp"이고,
    자원 경로는 "Resources/NoteApp"이고,
    의존 모듈은 NotesCore이고,
    의존 라이브러리는 HmjeUI이다.

앱 NoteApp의
    시작 모듈은 NotesApp이고,
    지원 플랫폼은
        iOS 17 이상과,
        macOS 14 이상이다.

NoteApp 앱의 iOS 설정의
    묶음 식별자는 "com.example.notes"이고,
    표시 이름은 "NoteApp"이고,
    아이콘 자원은 "Resources/NoteApp/iOS/AppIcon"이고,
    권한 파일은 "Resources/NoteApp/iOS/NoteApp.entitlements"이다.

모듈 NoteAppTests의
    원본 경로는 "Tests/NoteAppTests"이고,
    의존 모듈은 NotesCore이다.

시험 NotesCoreTests의
    시험 모듈은 NoteAppTests이고,
    시험 대상은 NotesCore이다.
```

## 3. 선언 종류와 속성

### 3.1 프로젝트

- `버전`: 프로젝트의 의미 버전 문자열입니다.

### 3.2 의존 프로젝트

- `깃 주소`와 `요구 버전`, 또는 `로컬 경로` 중 하나를 사용합니다.
- Git과 로컬 경로를 동시에 선언할 수 없습니다.
- 프로젝트 의존성은 원본 위치와 버전만 결정합니다. 모듈은 직접 의존 프로젝트가
  제공하는 라이브러리를 `의존 라이브러리`로 별도 선택해야 합니다.
- 전이 프로젝트의 라이브러리는 자동으로 노출되지 않습니다.
- 설치된 툴체인이 싣는 플랫폼 라이브러리(HmjeUI, HmjeNetwork, 표준·기반 …)는
  `의존 프로젝트` 없이 씁니다. 같은 `프로젝트` 이름의 로컬 의존 프로젝트를 선언하면 그
  플랫폼 복사본을 통째로 대신합니다([플랫폼 라이브러리](platform-libraries.md)).

### 3.3 모듈

- `원본 경로`: 생략 시 `Sources/<모듈 이름>`입니다. 여러 경로를 목록으로 줄 수 있습니다.
- `원본 파일`: 원본 경로 아래에서 명시한 파일만 고릅니다(생략하면 전부 자동 발견).
- `제외 원본 파일`: 자동 발견에서 뺄 파일(템플릿·데이터 원본) 목록입니다. 선택한 경로는
  선언한 원본 경로를 벗어날 수 없습니다.
- `자원 경로`: 선택 속성입니다.
- `의존 모듈`: 같은 프로젝트 안에서 직접 사용하는 모듈 목록입니다.
- `의존 라이브러리`: 직접 의존 프로젝트나 설치된 플랫폼 라이브러리가 제공하는
  라이브러리 목록입니다. 이
  선언은 라이브러리가 내보낸 모듈을 가져올 권한을 열 뿐, 모든 공개 모듈을 빌드
  또는 링크하지 않습니다.
- 실제 외부 모듈 의존성은 원본 파일의 `가져온다`에서 결정합니다. 선택한
  라이브러리가 내보낸 모듈 중 실제로 가져온 모듈과 그 의존 폐쇄만 산출물에
  포함됩니다.
- 기존 `의존성`은 파일-모듈 마이그레이션을 위해 유지되는 호환 문법입니다. 새
  프로젝트에서는 `의존 모듈`과 `의존 라이브러리`를 사용합니다.
- 원본 경로 아래 `.hmje` 파일은 상대 경로의 Unicode NFC 정렬 순서로 자동
  발견합니다. 선언의 의미는 파일 정렬 순서에 의존하지 않아야 합니다.
- 자원 경로 아래의 일반 파일은 그 자원 루트를 기준으로 한 논리 이름으로
  발견합니다. 예를 들어 자원 경로가 `assets`이면 원본의
  `파일 "icons/add.png"`가 실제 `assets/icons/add.png`를 가리킵니다. 실제 디렉터리
  이름은 소스 API에 노출되지 않습니다.
- 파일 데이터는 그 원본 파일과 같은 모듈이 선언한 자원만 사용할 수 있습니다.
  의존 모듈의 자원은 직접 참조할 수 없으며, 절대 경로와 `..`도 금지합니다.

### 3.4 앱과 실행 파일

- `시작 모듈`: `main.hmje`를 포함한 모듈입니다.
- `지원 플랫폼`: 빌드할 수 있는 플랫폼과 최소 버전 목록입니다.
- simulator/device와 debug/release는 선언하지 않고 빌드 목적지에서 선택합니다.
- `URL 스킴`(앱만): 이 앱을 여는 주소의 스킴 목록입니다(`"example"`, `"example-dev"` —
  `:`·`//` 없이 이름만). `앱 X의` 블록에 바로 쓰며, 모듈·라이브러리·실행 파일·시험
  블록과 플랫폼 설정 안에 쓰면 오류입니다. 스킴은 영어 소문자로 시작하고 소문자·숫자·
  `+`·`-`·`.`만 쓰며(64자 이하) `http`·`https`·`file`·`about`·`data`·`javascript`·
  `blob`은 쓸 수 없습니다. macOS·iOS는 Info.plist `CFBundleURLTypes`로, Windows는 첫
  실행에서 `HKCU\Software\Classes\<스킴>`으로 등록합니다. 앱 코드는 진입점의
  `URL이 열리면` 블록에서 받습니다([언어 명세](language.md) 「앱을 여는 주소」).

  ```hmjep
  앱 예제앱의
      시작 모듈은 예제앱진입이고,
      URL 스킴은 "example"와, "example-dev"이고,
      지원 플랫폼은 macOS 14 이상이다.
  ```

  따옴표 값 여럿은 한 줄 목록(`"a"와, "b"이다.`)으로도, 여러 줄 목록으로도 씁니다.
- `필요 기능`: 모듈·외부 구현·산출물이 요구하는 플랫폼 기능(`네트워크`, `오디오 입력`, `위치`,
  `카메라`) 목록입니다. 선택된 의존 그래프의 요구를 합쳐 Android 권한과 iOS·macOS
  `Info.plist` 사용 설명으로 옮깁니다([프로젝트 기능과 Android 권한](project.md)).
- `파일 공유`(Android 설정): `"1"`이면 앱이 임시 파일을 `content://` 주소로 다른 앱과
  나누도록 androidx `FileProvider`(권한 이름 `<묶음 식별자>.hmje.files`)를 APK manifest에
  선언합니다. FileProvider 클래스는 앱의 JVM 의존(androidx.core)이 제공해야 합니다.
- 프로젝트 CLI는 hosted `posix-host-*`, `macos-app`, `macos-launch-agent-pkg`,
  `ios-sim-*`, `android-*`, `windows-*`와
  semtleOS `app-*`/`aarch64-stdapp-*` 타깃에서 같은 모듈 그래프를 컴파일합니다.
  타깃에서 플랫폼을 유추할 수 있으면 `--platform`을 생략해도 해당 플랫폼 설정과
  자원 필터를 선택합니다.
- hosted POSIX와 macOS 타깃의 `--target-arch`는 `x86_64` 또는 `aarch64` thin
  산출물을 선택합니다. 생략하면 빌드 호스트 아키텍처를 사용합니다. Apple Silicon
  Mac에서도 Intel Mac용 `x86_64` 실행 파일·앱·패키지를 교차 빌드할 수 있습니다.
  macOS 호스트의 `posix-host-exe`·`macos-app`은 `--target-arch universal2`로 두 조각을
  `lipo`로 합친 산출물도 만듭니다([인텔 맥](building.md)).

### 3.5 라이브러리

- `내보내는 모듈`: 외부 프로젝트가 이름으로 가져올 수 있는 모듈 목록입니다.
- 모듈이 내보내져도 그 안의 `공개` 선언만 외부에 보입니다.
- 라이브러리는 외부 의존 경계이며 소스 래퍼 모듈이 아닙니다. 라이브러리 이름과
  공개 모듈 이름은 같을 필요가 없고, 하나의 라이브러리가 여러 모듈을 내보낼 수
  있습니다.

### 3.6 시험

- `시험 모듈`: 시험 코드를 담은 모듈입니다.
- `시험 대상`: 시험하는 모듈입니다.

### 3.7 외부 구현

외부 구현은 C, C++, Objective-C 또는 Objective-C++ 어댑터와 Apple Metal 원본을
프로젝트 경계 안에서 빌드합니다. 툴체인은 파일 이름이나 외부 함수 이름으로 구현을
추측하지 않습니다. 모듈에 붙인 외부 구현은 그 모듈을 사용하는 최종 산출물까지
전이되고, 산출물에 직접 붙인 외부 구현은 그 산출물에서만 사용합니다.

```hmjep
외부 구현 AuthNative의
    원본 경로는 "Native/Auth"이다.

    macOS 설정의
        원본 파일은
            "auth_macos.c"와,
            "auth_kernels.metal"이고,
        시스템 프레임워크는 Metal이다.

    Linux 설정의
        원본 파일은 "auth_linux.c"이고,
        시스템 라이브러리는 crypto이다.

모듈 Auth의
    원본 경로는 "Sources/Auth"이고,
    외부 구현은 AuthNative이다.
```

공통 속성은 모든 플랫폼에 적용하고, 선택된 중첩 플랫폼 설정을 그 위에 더합니다.
플랫폼 블록은 서로 독립된 문장 블록이므로 각각 `이다.`로 끝납니다. 공통 속성은
플랫폼 설정보다 앞에 둡니다.

- `원본 경로`: 외부 구현 파일의 프로젝트 상대 루트이며 필수입니다.
- `원본 파일`: 원본 경로에 상대적인 파일 목록입니다. 공통 또는 플랫폼 설정을
  합쳐 선택 플랫폼에서 하나 이상이어야 합니다. C 계열 원본은 목적 코드로
  컴파일해 실행 파일에 링크하고, Metal 원본은 별도 Metal 파이프라인으로 보냅니다.
- `헤더 경로`: 원본 경로에 상대적인 include 디렉터리 목록입니다.
- `컴파일 정의`: `NAME` 또는 `NAME=VALUE` 형식의 전처리기 정의입니다.
- `시스템 라이브러리`: 링커가 `-l<이름>`으로 찾는 시스템 라이브러리 목록입니다.
- `시스템 프레임워크`: Apple 플랫폼에서 링크할 프레임워크 목록입니다.

지원 확장자는 `.c`, `.cc`, `.cpp`, `.cxx`, `.m`, `.mm`, `.metal`입니다. `.metal`은
Apple 플랫폼 전용입니다. 선택된 Metal 원본 전부를 `metal`로 AOT 컴파일한 뒤
제품마다 하나의 `<제품 이름>.metallib`로 링크합니다. macOS 원시 실행 파일에서는
`<실행 파일 이름>.assets` 안에, macOS 앱에서는 `Contents/Resources/assets` 안에,
iOS 앱에서는 번들 루트에 둡니다. 같은 논리 경로의 일반 자원이 생성된 Metal
라이브러리를 덮어쓸 수 없습니다.

Metal 원본에는 외부 구현의 헤더 경로와 컴파일 정의를 C 계열 원본과 똑같이
적용합니다. 선택된 Metal 원본이 있으면 C/Objective-C 어댑터에는 컴파일러 전용
문자열 정의 `HMJE_PROJECT_METALLIB_NAME`을 제공합니다. 값은 실제 산출물 파일명이며,
어댑터가 번들 또는 실행 파일 옆에서 라이브러리를 찾을 때 씁니다. 프로젝트나
명령줄에서 이 이름을 다시 정의하면 오류입니다. Metal API를 호출하는 어댑터의 시스템
프레임워크 선언과 metallib 로딩·함수 선택은 라이브러리 구현의 책임입니다.

Swift 외부 구현, 임의 셸 명령과 원시 컴파일러·링커 인수는 v0에 없습니다. 동적인 빌드
값은 manifest에 환경 변수 보간을 넣지 않고 `hmjec --define NAME=VALUE`로 전달합니다.
모든 원본과 헤더 경로는 선언 프로젝트 경계를 벗어날 수 없습니다.

### 3.8 iOS 앱 설정과 자원

- `묶음 식별자`와 `표시 이름`은 생성된 앱의 `Info.plist`에 반영합니다.
- `지원 플랫폼`에서 계산한 유효 최소 iOS 버전은 LLVM target triple과 앱의
  `MinimumOSVersion`에 함께 반영합니다.
- `아이콘 자원`은 프로젝트 상대 파일, `.xcassets`, `.appiconset` 또는
  `Contents.json`을 가진 app-icon 디렉터리를 가리킵니다. asset catalog는 `actool`로
  컴파일합니다.
- `권한 파일`은 프로젝트 상대 plist 파일입니다. Simulator에서는 XML로 정규화해
  Mach-O의 `__TEXT,__entitlements` 섹션에 넣고, macOS ad-hoc 코드 서명에는
  iOS 권한을 전달하지 않습니다. 실기기에서는 provisioning 검증 후 코드 서명에
  전달합니다. Keychain 같은 서비스에는 앱에 맞는 `application-identifier`와
  Keychain access group이 필요합니다. Simulator 전용 합성 접두사를 실기기
  provisioning 식별자로 사용해서는 안 됩니다.
- 선택된 모듈의 자원은 논리 경로를 보존해 앱 번들에 복사합니다. 대소문자를 무시한
  논리 경로가 겹치거나 `Info.plist`·실행 파일과 충돌하면 빌드 오류입니다.
- HmjeGraphics 폰트는 자원 루트의 `fonts` 논리 경로로 앱 번들에 들어가며
  런타임도 같은 경로를 사용합니다. 프로젝트 빌드는 선언하지 않은 외부 저장소나
  현재 작업 디렉터리에서 폰트를 보충하지 않습니다. 프로젝트 그래프가 없는 레거시
  단일 파일 빌드만 `HMJE_UI_ROOT` 호환 경로를 사용할 수 있습니다.

### 3.9 macOS headless LaunchAgent 패키지

`macos-launch-agent-pkg` 타깃은 화면 표시가 없는 앱·실행 파일을 로그인 사용자의
장기 실행 Agent로 설치하는 로컬 Installer 패키지를 만듭니다. 화면 표시 함수를 쓰는
산출물은 이 타깃에서 거절합니다. HmjeUI나 AppKit을 제품에 자동으로 추가하지 않습니다.

- 기존 macOS 설정의 `묶음 식별자`를 launchd label과 앱 bundle identifier로,
  `표시 이름`, `아이콘 자원`, `권한 파일`을 내장 headless 앱에 그대로 사용합니다.
- `/Applications/<이름>.app`과 `/Library/LaunchAgents/<묶음 식별자>.plist`를 설치하며,
  `KeepAlive`와 `ProcessType=Background`로 장기 실행합니다.
- Agent 시작 전에 작업 디렉터리와 `HMJE_AGENT_DATA_DIR`을 로그인 사용자의
  `~/Library/Application Support/<묶음 식별자>`로 맞춥니다. 상대 DB 경로는 launchd의
  임의 작업 디렉터리에 의존하지 않습니다.
- LaunchAgent 실행에서는 `~/.local/bin`, `~/bin`, Apple Silicon·Intel Homebrew와
  시스템 실행 경로를 순서대로 설정하고, 같은 Application Support 디렉터리의
  `agent.stdout.log`·`agent.stderr.log`에 진단 출력을 보존합니다. 같은 번들의
  실행 파일을 MCP stdio 모드로 직접 호출할 때는 표준입출력을 바꾸지 않습니다.
- 설치 후 현재 GUI 사용자 domain에 bootstrap을 시도합니다. GUI 사용자가 없으면 다음
  로그인 때 시스템 LaunchAgents 디렉터리에서 로드됩니다.
- 앱의 `Contents/Resources/assets/uninstall.command`는 Agent와 설치 파일만 제거하고
  데이터베이스·작업 자료는 보존합니다.
- `.app` payload는 ad-hoc 서명·검증합니다. 로컬 `.pkg` 자체의 Developer ID 서명과
  notarization은 배포 인증서가 필요한 별도 release 단계입니다.

```sh
hmjec --project . --artifact Agent --target macos-launch-agent-pkg \
  --target-arch aarch64 -o build/aarch64/Agent.pkg
hmjec --project . --artifact Agent --target macos-launch-agent-pkg \
  --target-arch x86_64 -o build/x86_64/Agent.pkg
sudo installer -pkg build/aarch64/Agent.pkg -target /
sh /Applications/Agent.app/Contents/Resources/assets/uninstall.command
```

각 패키지는 지정한 단일 아키텍처의 thin payload입니다. Universal 2 병합은 이 타깃이
자동으로 수행하지 않으므로, 배포 대상별 패키지를 각각 보관합니다.

### 3.10 macOS 앱 설정과 번들

`macos-app` 타깃은 macOS hosted 실행 파일을 표준 번들 구조로 묶습니다.

```text
NoteApp.app/
    Contents/
        Info.plist
        MacOS/NoteApp
        Resources/assets/
```

- macOS 설정의 `묶음 식별자`, `표시 이름`, `아이콘 자원`, `권한 파일`을 iOS와
  같은 프로젝트 속성으로 읽습니다. 새 manifest 문법은 없습니다.
- 지원 플랫폼의 최소 macOS 버전은 네이티브 컴파일·링크와
  `LSMinimumSystemVersion`에 함께 반영합니다.
- 파일 아이콘은 `Contents/Resources`에 복사하고 asset catalog는 `actool`의
  `macosx` 플랫폼으로 컴파일합니다.
- 모듈 자원과 생성된 Metal 라이브러리는 `Contents/Resources/assets`에 둡니다.
- 기본은 재현 가능한 ad-hoc 서명입니다. `HMJE_MACOS_SIGNING_IDENTITY`에
  Developer ID Application 등의 서명 식별자를 주면 그 인증서와
  hardened runtime(`codesign --options runtime`), secure timestamp를 사용합니다.
  `HMJE_MACOS_HARDENED_RUNTIME=1`은 ad-hoc 서명에서도 hardened runtime 검증을
  켤 수 있고, `0`은 실제 인증서의 기본값을 끄는 명시적 예외입니다.
- `HMJE_MACOS_PROVISIONING_PROFILE=<경로>`는 기존 macOS 프로필을
  `Contents/embedded.provisionprofile`에 서명 전에 넣습니다. `security cms -D`로
  해독하여 Platform(OSX/macOS), ExpirationDate, TEAM.묶음식별자(끝의 `*` 허용),
  DeveloperCertificates의 선택 인증서 SHA-1, 요청 권한의 허용 값들을 검증합니다.
  인증서 선택은 `security find-identity -v -p codesigning`의 정확한 이름 또는
  SHA-1을 씁니다(이름이 중복되면 SHA-1 필요). 프로필을 자동으로 만들거나 계정을
  변경하지 않습니다. 프로필의 앱/팀 식별자를 서명 권한에 구체화합니다.
- keychain-access-groups, application-identifier, com.apple.application-identifier,
  com.apple.developer.*, application-groups 등 제한 권한을 ad-hoc으로 빌드하면
  경고를 내고 빌드는 계속합니다. macOS 실행에는 개발 인증서와 일치하는 프로필을
  위 두 환경 변수로 지정해야 합니다. 실제 인증서로 제한 권한을 서명하면서 프로필을
  생략하거나 맞지 않는 프로필을 주면 빌드 오류입니다.
- 제한 권한/ad-hoc 경고는 stderr에 `HMJE_MACOS_SIGNING_DIAGNOSTIC\t<JSON>`도
  출력합니다. JSON의 `code=restricted-entitlements-adhoc`, `launchAllowed=false`,
  `restrictedKeys`, `message`를 소비자는 실행 방지에 쓸 수 있습니다. 빌드마다 새로
  출력하므로 지난 산출물의 진단을 재사용하지 않습니다.
- macOS 서명 보조 도구는 배포 툴체인에 포함됩니다. IDE는 선택한 `hmjec`의
  `--macos-signing resolve|catalog|amfi`를 사용하므로 앱에 실린 툴체인에도 같은
  검증이 적용됩니다. 설치/빌드에는 Python이나 Swift 인터프리터가 필요하지 않습니다.
- 권한 파일이 있으면 서명에 전달하고 `codesign --verify --deep --strict`
  검증까지 성공해야 제품을 출력 경로에 게시합니다.

### 3.11 원시 실행 파일의 자원

macOS·Linux·Windows의 원시 실행 파일은 자원을 실행 파일과 같은 디렉터리의
`<실행 파일 이름>.assets`에 둡니다. Windows의 `.exe` 확장자는 자원 디렉터리
이름에서 제외합니다.

```text
NoteApp
NoteApp.assets/
    fonts/
    unicode/

NoteApp.exe
NoteApp.assets/
```

자원 디렉터리 안에서는 모듈이 선언한 자원 루트 기준 논리 경로를 그대로 보존합니다.
여러 모듈이 대소문자만 다른 동일 논리 경로를 내보내면 패키징 오류입니다. 생성된 Metal
라이브러리도 macOS 원시 실행 파일에서는 이 디렉터리에 함께 들어갑니다. 원시 실행
파일이나 자원 디렉터리 중 하나만 옮기는 것은 완전한 제품 이동이 아닙니다.

### 3.12 앱 아이콘

앱 아이콘은 플랫폼 설정의 `아이콘 자원` 하나로 적습니다. 경로는 프로젝트 기준입니다.

```hmjep
NoteApp 앱의 macOS 설정의
    아이콘 자원은 "AppIcon/macOS/AppIcon.icns"이다.

NoteApp 앱의 iOS 설정의
    아이콘 자원은 "AppIcon/iOS/AppIcon.appiconset"이다.

NoteApp 앱의 Android 설정의
    아이콘 자원은 "AppIcon/Android"이다.

NoteApp 앱의 Windows 설정의
    아이콘 자원은 "AppIcon/Windows/AppIcon.ico"이다.
```

| 플랫폼 | 받는 꼴 | 제품에서 |
| --- | --- | --- |
| macOS | 파일(`.icns` 등) 또는 `.xcassets`·`.appiconset` | 파일은 `Contents/Resources`에 복사하고 `CFBundleIconFile`(Xcode 없이 빌드됩니다), 카탈로그는 `actool --platform macosx` |
| iOS | `.png`·`.jpg` 파일 또는 `.xcassets`·`.appiconset` | 카탈로그는 `actool`(단일 1024 · 다크·색조 모양 포함) |
| Android | `.png` 파일 또는 아이콘 폴더 | 아래 |
| Windows | `.ico` | 조정기가 `.res`(RT_ICON 그림마다 + RT_GROUP_ICON 이름 1)를 써서 실행 파일과 함께 링크합니다. `.ico`가 아니면 빌드 오류 |

Android는 PNG 파일 하나면 그 그림을 레거시 mipmap과 적응형 전경으로 함께 쓰고 배경은 기본 그라데이션입니다.
폴더를 주면 아래 정해진 이름만 res에 옮깁니다(다른 파일은 APK에 들어가지 않습니다).

```text
AppIcon/Android/
    mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/hmje_app_icon.png   레거시(API 26 미만), 하나 이상
    drawable-nodpi/hmje_app_icon_foreground.png                  적응형 전경(108dp, 안전 영역 66dp)
    drawable-nodpi/hmje_app_icon_background.png                  적응형 배경(없으면 기본 그라데이션)
    drawable-nodpi/hmje_app_icon_monochrome.png                  테마 아이콘 — API 33 이상 platform jar일 때만
```

전경이 있으면 조정기가 `mipmap-anydpi-v26/hmje_app_icon.xml`(adaptive-icon)을 쓰고, manifest의
`android:icon`·`android:roundIcon`은 `@mipmap/hmje_app_icon`입니다. 훈민정음 IDE의 앱 아이콘 편집기(자산 탭)는
원본 한 장에서 네 플랫폼 꼴을 모두 만들고 이 설정을 적습니다.

## 4. 가져오기

외부 또는 다른 모듈은 식별자로 가져옵니다.

```hmje
HmjeUI를 가져온다.
```

따옴표 경로는 파일 가져오기입니다.

```hmje
"legacy/widget.hmje"를 가져온다.
```

- 같은 모듈의 파일은 자동 발견되므로 서로 가져올 필요가 없습니다.
- 모듈 가져오기는 적힌 파일에만 적용합니다. 같은 모듈의 다른 파일이 가져온 외부
  모듈은 자동으로 보이지 않습니다.
- 같은 프로젝트 모듈은 `의존 모듈`에, 외부 프로젝트의 모듈은 그 모듈을
  내보내는 라이브러리를 `의존 라이브러리`에 먼저 선언해야 합니다.
- 라이브러리 의존만으로 공개 모듈 전체를 가져오거나 링크하지 않습니다. 각 파일의
  명시적 모듈 가져오기가 실제 빌드 의존성을 만듭니다.

### 4.1 UI DSL 계약의 정의 위치

프로젝트 모드에서 컴파일러는 경로나 `HmjeUI`라는 모듈 이름을 추측하지 않습니다.
UI DSL을 쓰는 파일에 보이는 유일한 공개 `화면` 특성이 숨은 UI ABI의 정의 위치입니다.
색상 토큰 계약은 그 공개 `화면` 특성의 원본 파일에서 정상적으로 보이는 유일한
`시스템색상종류` 선언에서 읽습니다. 따라서 `화면`은 `HmjeUI`에, 색상은 그 파일이
가져온 `HmjeGraphics`에 있어도 모듈 경계가 유지됩니다. 로컬 및 Git 의존성은
lock/프로젝트 해석이 고른 바로 그 원본으로 닫히며 `HMJE_UI_ROOT`가 필요하지 않습니다.
계약 선언이 없거나, 여러 원본 단위에 중복되거나, 명시적 정수 값 없는 케이스를
가지면 프로젝트 빌드는 실패합니다.

`HMJE_UI_ROOT` 경로 탐색은 프로젝트 그래프가 없는 레거시 단일 파일 빌드에서만
호환 경로로 남습니다.
- 자기 모듈의 선언은 가져오기 없이 같은 모듈의 모든 파일에서 보입니다.
- 프로젝트 모드의 파일 경로는 현재 모듈 원본 경계를 벗어날 수 없습니다.
- 단일 파일 모드는 기존 상대 경로 가져오기 의미를 유지합니다.
- import하지 않은 모듈과 전이 의존 모듈의 선언은 보이지 않습니다.
- 보이는 모듈들의 동명 함수는 하나의 overload 후보군을 이룹니다. 서로 다른
  signature는 모두 유지하고, 같은 호출에 둘 이상의 후보가 똑같이 맞을 때만
  모호성 오류입니다.
- 함수가 아닌 동명 선언은 모호성 오류입니다. v0에는 별칭이나 자동 재수출이 없습니다.

### 4.2 표준 자동 제공과 플랫폼 런타임

일반 프로젝트와 컴파일러 자체는 `library/Project.hmjep`에 선언된 `표준` 모듈의
**공개 선언 전체**를 가져오기 없이 사용합니다. 문자열·배열·특성·출력뿐 아니라
해시·사전·집합·메모리·버퍼뷰도 포함됩니다. 표준 원본은 manifest의 원본 경로,
선택·제외 파일 및 플랫폼 조건으로 결정하며, 프로젝트 조정기와 직접 프론트엔드가
같은 선택기를 사용합니다. 컴파일러 소스도 표준 파일 경로를 직접 가져오지 않습니다.

- 표준의 모듈 내부·프로젝트 내부·파일 내부 선언은 각 접근 범위를 유지합니다.
  런타임을 위해 먼저 읽은 파일의 내부 도우미가 앱에 공개되지는 않습니다.
- `기반`, `DB`, `SQLite`, `Backend` 등 다른 모듈은 명시적으로 가져옵니다.
  의존성 그래프에 존재하는 것만으로 그 모듈의 선언이 파일에 보이지 않습니다.
- 플랫폼 런타임은 target이 요구하는 구현 파일을 선택합니다. 이것은 표준 공개 API와
  별개이며 숨은 구현 파일을 일반 모듈처럼 자동 재수출하지 않습니다.
- 커널과 raw freestanding 프로필은 사용자 공간 syscall을 자동 주입하지 않고
  해당 ABI의 명시적 런타임 선택을 유지합니다. 일반 semtleOS 앱과
  `aarch64-stdapp-*` 프로젝트는 전체 표준 자동 제공 계약을 따릅니다.

집계 원본의 실제 프로젝트·모듈 소유권과 암묵 모듈 표시는 접근 검사에 전달합니다.
원시 파싱 캐시는 접근 권한을 저장하지 않으며 manifest 선택이 바뀌면 유효 모듈
지도를 다시 적용합니다. 자세한 계약과 네이티브 검증은
[표준 라이브러리 자동 제공](../library/Project.hmjep)을 따릅니다.

## 5. 접근 수준

접근 수준을 생략하면 `모듈 내부`입니다.

| 접근 수준 | 범위 |
|---|---|
| `타입 내부` | 해당 명목 타입 본문의 초기화 함수와 멤버 함수 |
| `파일 내부` | 선언된 source unit |
| `모듈 내부` | 같은 모듈 |
| `프로젝트 내부` | 같은 프로젝트에서 의존 관계가 있는 모듈 |
| `공개` | 모듈을 가져온 외부 프로젝트 |

- `타입 내부`는 타입 멤버에만 쓸 수 있습니다.
- 공개 타입의 멤버도 자동 공개되지 않습니다.
- 공개 API가 더 좁은 접근 수준의 타입·특성·열거형을 노출하면 오류입니다.
- 서로 다른 파일의 `파일 내부` 동명 선언은 공존할 수 있어야 합니다.
- extension은 v0에 없고, 도입하더라도 `타입 내부` 접근을 자동으로 얻지 않습니다.

접근 수준은 자신이 제한하는 선언 명사 바로 앞에 씁니다. effect, 반환,
visibility와 linkage는 독립 속성입니다.

```hmje
위험 공개 함수 포인터열기는 ...
실패할 수 있는 프로젝트 내부 함수 연결은 ...
실패할 수 있고 위험한 파일 내부 함수 읽기는 ...
```

외부 구현과 링크 심볼 별칭은 후행합니다.

```hmjep
프로젝트 내부 함수 pthread_create는 (...)를 받아 정수32로 반환하며 외부에서 구현된다.
위험 프로젝트 내부 함수 스레드생성은 (...)를 받아 정수32로 반환하며 외부에서 "pthread_create"라는 이름으로 구현된다.
```

플랫폼 callback 기반 함수는 같은 후행 외부 구현 문법에 `비동기` 효과를 조합합니다.
컴파일러는 이를 native 함수를 coroutine으로 오인하지 않고, 숨은 HMJE coroutine
wrapper와 완료 token을 만듭니다. native 시작 심볼은 선언한 POD 인자 뒤에 `uint64_t`
token을 받고 즉시 반환해야 하며, 나중에 `async_host_runtime.h`의 완료 함수로 결과를
게시합니다. callback에서 HMJE 함수나 coroutine resume을 직접 호출하는 것은 ABI상
허용하지 않습니다. 현재 host 구현 대상은 macOS/Linux POSIX와 iOS이며 semtleOS native
waker는 별도로 연결해야 합니다.

## 6. 명목 타입 초기화

```hmje
공개 구조체 사용자정보의
    타입 내부 상수 식별자는 자연수이고,
    프로젝트 내부 상수 캐시키는 자연수이고,
    공개 변수 이름은 문자열이다.

    공개 초기화는 (이름: 문자열)을 받아
        자신의 식별자는 새식별자()이고,
        자신의 캐시키는 새캐시키()이고,
        자신의 이름은 이름이다.
```

- 초기화는 반환 전에 모든 저장 필드를 정확히 초기화합니다.
- 완전 초기화 전 `자신`을 외부로 유출하거나 일반 멤버 함수를 호출할 수 없습니다.
- 명시적 `초기화`가 하나라도 있으면 모든 일반 생성은 매개변수 이름과 타입이
  맞는 초기화를 거쳐야 합니다. 필드 리터럴로 초기화를 우회할 수 없습니다.
- 명시적 `초기화`가 없을 때만 기존 구조체 리터럴을 사용할 수 있고, 호출자가
  모든 필드에 접근할 수 있어야 합니다. 이 리터럴의 실효 접근 수준은 가장 제한적인
  필드와 같습니다.
- 초기화 구현이 모든 필드를 채운 뒤 값을 만드는 컴파일러 내부 리터럴은 위 우회
  금지의 대상이 아닙니다.

## 7. 구현 순서와 호환

1. 프로젝트·모듈·source unit·선언·타입 identity
2. 정적 `Project.hmjep` parser와 module graph
3. 접근 수준과 호출자 문맥을 받는 resolver
4. HMIR/LLVM canonical link identity
5. local dependency 종단 빌드
6. Git/lock/native/resource
7. HmjeUI 소스 라이브러리 포팅

기존 overlay와 umbrella 파일은 모든 소비자가 전환될 때까지 제거하지 않습니다.

## 8. 현재 프로젝트 CLI

로컬 의존성 graph를 포함한 프로젝트는 다음 공개 명령으로 generic HMIR 또는
LLVM IR까지 컴파일합니다.

```sh
hmjec --project . --artifact HmjeLLM --target hmir -o build/HmjeLLM.hmir
hmjec --project . --artifact HmjeLLM --target llvm-ir -o build/HmjeLLM.ll
```

앱·실행 파일 산출물은 시작 모듈의 인자 없는 `앱시작` 함수를 진입점으로 선택합니다.
컴파일러는 namespace lowering 뒤 그 정확한 backend symbol을 호출하는 최상위 shim을
합성하므로, 숨은 Runtime 원본이 개발자 모듈을 역으로 가져오지 않습니다.

```sh
hmjec --project . --artifact MyServer --target posix-host-llvm-ir -o build/server.ll
hmjec --project . --artifact MyServer --target posix-host-exe -o build/server
hmjec --project . --artifact MyApp --target macos-app -o build/MyApp.app
hmjec --project . --artifact MyAgent --target macos-launch-agent-pkg \
  --target-arch x86_64 -o build/x86_64/MyAgent.pkg
hmjec --project . --artifact MyApp --target ios-sim-llvm-ir -o build/app-ios.ll
hmjec --project . --artifact MyApp --target ios-sim-app -o build/MyApp.app
```

`--project`가 없으면 기존 단일 `.hmje` 파일 CLI 의미를 그대로 유지합니다. 현재
프로젝트 CLI의 `hmir`/`llvm-ir`은 플랫폼 ABI를 붙이지 않는 검증된 generic
산출물입니다. `posix-host-*`와 `ios-sim-*`은 같은 `CompilationPlan`과 HMIR을 기존
플랫폼 emitter/linker에 연결합니다. 레거시 compiler-owned import는 제품 이름이 아니라
선택된 source의 논리 경로로 내부 Runtime capability를 추론합니다. 이 추론은 정식
stdlib/runtime 모듈 전환이 끝나면 quoted import와 함께 제거합니다.
