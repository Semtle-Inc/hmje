# Android Java 소스와 고정 아카이브 의존성

Android APK의 `외부 구현`은 `원본 파일`에 `.java`와 `.android.lock.json`을 선언할 수 있습니다. C·C++는 기존 컴파일 경로를 사용합니다. `android-exe` 산출물은 Java 입력을 거부합니다. Java 및 아카이브 의존성 처리는 제품 조정기와 Android 네이티브·SDK 패키징 단계에서 수행하며 HMJE 프런트엔드와 LLVM ABI를 변경하지 않습니다.

```hmjep
외부 구현 ServiceNative의
    원본 경로는 "Native"이다.
    Android 설정의
        원본 파일은
            "service.c"와,
            "ServiceBridge.java"와,
            "dependencies.android.lock.json"이다.
```

## 잠금 파일

버전 1 잠금 파일은 `roots`와 `artifacts`를 포함합니다. 각 아카이브 항목은 정확한 `group:artifact:version` 식별자, 로컬 `path`, `sha256`, `type`과 의존성 식별자를 선언합니다. `type`은 `jar`·`aar` 또는 메타데이터 전용 `pom`입니다.

선택적인 `url`은 출처 기록용이며 패키징 도구가 해당 주소에 접속하지 않습니다. `minD8Version`은 호환되는 D8의 최소 버전을 지정합니다. 전이 의존성도 모두 잠금 파일에 있어야 하며 버전 충돌은 거부합니다. 빌드에는 Maven·Gradle 의존성 해석기가 없습니다.

```json
{"version":1,"minD8Version":"8.6.17","roots":["example:sdk:1.0"],
 "artifacts":[{"id":"example:sdk:1.0","type":"aar",
 "path":"maven/sdk-1.0.aar","sha256":"<64 lowercase hex digits>",
 "dependencies":[]}]}
```

## 입력 검증과 실행 경계

아카이브 경로는 심볼릭 링크를 해석한 뒤에도 잠금 파일의 디렉터리 안에 있어야 합니다. 툴체인의 Java 보조 도구는 경로 이탈·중복 항목·운영체제 간 압축 해제 경로 충돌·중복 클래스·패키지 자산 및 네이티브 라이브러리 충돌을 거부합니다.

아카이브 크기·압축 해제 크기·항목 수에 상한을 적용하고 CRC와 SHA-256을 검증합니다. XML 외부 엔티티와 javac 애너테이션 처리는 비활성화합니다. 의존성 코드는 Android용으로 컴파일하고 DEX로 변환하며 호스트 보조 도구에서 로드하지 않습니다. SDK 도구에는 인자 배열을 전달합니다. 빌드 중 네트워크 요청·플러그인 실행·임의의 Gradle 스크립트 평가는 수행하지 않습니다.

## 리소스와 패키징

AAR 리소스는 의존되는 라이브러리를 먼저 처리하는 순서로 컴파일하고 연결합니다. 라이브러리 매니페스트의 패키지마다 R 클래스를 생성하며, Java 8 람다 컴파일에는 SDK의 `core-lambda-stubs.jar`을 사용합니다. D8에는 모든 프로그램 클래스를 전달합니다. 코드 축소·난독화는 수행하지 않고 생성된 모든 DEX 파일을 패키지에 포함합니다.

`resources.arsc`는 압축하지 않은 형태로 저장하여 zipalign이 Android R 이상의 설치 조건을 충족하도록 합니다. APK 생성 시 캐시된 HMJE IR을 사용하더라도 입력 검증과 패키징을 다시 수행합니다. 아카이브 내부 파일이 바뀌었을 때 이전 APK를 재사용하지 않습니다.

## 매니페스트 병합과 지원 범위

매니페스트 병합은 키가 있는 컴포넌트·메타데이터·권한, 초기화 제공자 목록의 결합, 동일한 인텐트 필터, `${applicationId}`, `tools:node=merge`, 린트 전용 `tools:ignore`를 지원합니다. 앱 속성이 우선하며 호환되지 않는 라이브러리 속성은 앱에서 명시적으로 선언해야 합니다. 대상 API가 31 이상이면 인텐트 필터가 있는 컴포넌트에 `android:exported`를 명시해야 합니다.

라이브러리의 minSdk·minCompileSdk와 메타데이터 형식을 검사합니다. 지원하지 않는 tools 지시자·자리표시자·SDK 확장·핵심 라이브러리 디슈거링 요구는 진단과 함께 실패합니다. 지원 범위는 위 항목에 한정되며 Gradle 매니페스트 병합기의 전체 기능을 제공하지 않습니다.

AAR prefab·네이티브 빌드 스크립트, Kotlin·AIDL 소스 컴파일, ARM64 외의 APK는 이 경로에서 지원하지 않습니다. 미리 컴파일한 AAR 네이티브 라이브러리는 대상과 일치하는 arm64-v8a ELF 파일을 제공해야 합니다.

JAR의 서비스 제공자 목록은 병합합니다. 소비자 ProGuard 규칙과 고지는 META-INF에 유지합니다. 코드를 축소하지 않으므로 리플렉션으로 참조하는 클래스를 축소 단계에서 제거하지 않습니다. AAR 기준 프로파일 텍스트는 메타데이터로 보존하지만 `assets/dexopt`용 프로파일 컴파일은 구현하지 않았습니다. 의존성의 애너테이션·린트 도구도 실행하지 않습니다. D8이 제삼자 코드의 잘못된 디버그 지역 변수 표를 진단하더라도 호환되지 않는 Kotlin 메타데이터 버전을 허용하지 않습니다.

## D8 호환성

Kotlin 2.1에는 D8 8.6.17 이상이 필요하며 Gradle 없이 D8을 사용하는 경우에도 같습니다. [Android 컴파일러 호환성 표](https://developer.android.com/build/kotlin-support)를 참고하십시오.

설치된 SDK 35에는 더 오래된 개발 버전의 D8이 들어 있을 수 있습니다. 개발 빌드의 `HMJE_ANDROID_D8_OVERRIDE=/absolute/path/to/d8`에 설치된 호환 SDK 도구를 지정할 수 있습니다. 패키징 도구는 선언된 최소 버전을 검사하며 새 버전을 내려받거나 원격 버전을 선택하지 않습니다.

## 네이티브 UI 연결

네이티브 UI SDK 어댑터는 `hmje_android_java_vm()`과 `hmje_android_current_activity(JNIEnv*)`를 호출할 수 있습니다. 후자는 빌린 전역 포인터가 아니라 호출자가 소유하는 로컬 참조를 반환합니다. 호출자는 작업자 스레드를 연결하고 로컬 참조를 삭제하며, UI 스레드에서 Activity 수명을 다시 확인해야 합니다.

앱의 Java 클래스에는 Activity 클래스 로더를 사용합니다. 작업자 스레드의 `FindClass`는 다른 로더를 사용할 수 있습니다. 전경 Activity가 필요 없는 서비스에는 ApplicationContext 접근 함수를 사용할 수 있습니다.

Windows 네이티브 툴체인도 선언된 Java·JAR·AAR APK 산출물을 지원합니다. [데스크톱 호스트 요구 사항](android-hosts.md)을 참고하십시오. Java 보조 도구는 실행 중인 JDK에서 `javac`를 찾고 배치 래퍼 대신 SDK의 D8 JAR을 직접 실행합니다.
