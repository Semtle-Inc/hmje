# 소스 빌드

HMJE 소스에서 컴파일러와 명령줄 툴체인을 빌드합니다. 초기 컴파일러는 호스트·아키텍처가 같은 **1.0.0-beta.1 Release 툴체인** 또는 별도 `hmje-swift-seed` 저장소에서 구축한 컴파일러를 사용합니다.

| 시작 경로 | 용도 | 안내 |
| --- | --- | --- |
| Release 툴체인 | 설치한 HMJE로 공개 소스를 빌드 | 아래 운영체제별 절차 |
| Swift 씨앗 | Swift에서 시작해 Beta 1 컴파일러를 재현하고 공개 소스를 빌드 | [Swift 부트스트랩](bootstrap.md) |

이 저장소의 빌드 대상은 컴파일러·런타임·명령줄 도구와 공개 라이브러리입니다. IDE는 별도 Release 바이너리로 제공합니다.

## macOS·Linux

Git, POSIX 셸, clang과 운영체제 SDK/개발 헤더가 필요합니다.
macOS는 Command Line Tools의 Apple clang을 사용하며 배포 대상은 기본 macOS 14.0입니다.
iOS 대상과 Metal 원본(hmje-sem, hmje-llm)에는 Xcode가 필요합니다.
Linux는 clang, OpenSSL 3 개발 헤더, libX11 개발 헤더가 필요하고 Release 툴체인은
glibc 2.38 이상에서 실행됩니다(Ubuntu/Debian: `sudo apt install clang build-essential libssl-dev libx11-dev`,
Fedora: `sudo dnf install clang gcc openssl-devel libX11-devel`). 운영체제별 준비물은
[릴리스 노트](../RELEASE_NOTES-1.0.0-beta.1.md#설치-전-준비물)에 정리되어 있습니다.

Releases 툴체인을 원하는 별도 폴더에 설치하고 그 경로를 `RELEASE_PREFIX`로
지정합니다. 기존 설치에 덮어쓰지 않습니다. 아래는 저장소 루트에서 실행합니다.

```sh
export RELEASE_PREFIX="$HOME/hmje-release-1.0.0-beta.1"
clang --version
sh tools/build-from-release.sh "$RELEASE_PREFIX/lib/hmje/libexec/bin" "$PWD/build/native-bin"
HMJE_INSTALL_WARM=0 sh tools/install-native.sh "$PWD/build/install" "$PWD/build/native-bin"
"$PWD/build/install/bin/hmjec" --version
HMJEC="$PWD/build/install/bin/hmjec" sh tools/test.sh
```

사용할 Clang을 명시해야 하는 환경에서는 `HMJE_CLANG`에 실행 파일 경로를 지정합니다. macOS 빌드 드라이버는 기본적으로 `xcrun --sdk macosx --find clang`으로 선택합니다. 설치된 SDK와 호환되는 컴파일러를 사용하십시오.

`build-from-release.sh`는 지정한 도구 폴더를 초기 컴파일러로 사용하고
공개 프로파일·오프라인 라이브러리를 지정해 `build-native.sh`를 실행합니다.
이 트리의 `libraries/index.tsv`와 공개 lock이 정확히 맞아야 빌드합니다.
설치는 새 prefix를 요구하며 `HMJE_INSTALL_WARM=0`으로 캐시 예열을 생략합니다.
소스 빌드는 새 툴체인을 만드는 단계이며 기존 Release 설치를 변경하지 않습니다.
출력과 임시 설치가 필요 없으면 자신이 만든 `build/` 폴더를 지우면 됩니다.

## Windows 11

LLVM clang(`winget install LLVM.LLVM`), Windows SDK와 MSVC 개발 환경(Visual Studio
Build Tools의 C++ 데스크톱 개발)을 준비하고 호스트에 맞는 Release 툴체인을 먼저 설치합니다.
툴체인 설치 뒤에 LLVM을 설치했다면 `Install.cmd`를 다시 실행하거나 `HMJE_WINDOWS_CLANG`에
`clang.exe` 경로를 지정합니다. PowerShell에서 저장소 루트 기준으로 실행합니다.

```powershell
$env:HMJE_PLATFORM_PROFILE = 'public'
$env:HMJE_PLATFORM_OFFLINE = '1'
$env:HMJE_INSTALL_WARM = '0'
.\tools\build-windows-native.ps1 -Hmjec 'C:\HMJE\bin\hmjec.exe' -OutputDirectory '.\build\native-bin' -TargetArch aarch64
.\tools\install-windows-native.ps1 -NativeBinDirectory '.\build\native-bin' -Prefix '.\build\install' -NoActivate
```

x64에서는 `-TargetArch x86_64`를 사용합니다. POSIX 스모크 실행기는 macOS·Linux용이며,
Windows에서는 `.hmje` 예제를 `--target windows-exe`로 각각 빌드·실행합니다.
여기에 적은 Windows 명령은 macOS에서 실행 검증한 명령이 아닙니다.
