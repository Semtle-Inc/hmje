# Gemma 4 텍스트 추론 API

[inference.hmje](../Sources/HmjeLLM/inference.hmje)는 Gemma 4 E2B-IT QAT Q4_0 GGUF에 맞춘 최소 텍스트 디코더입니다. 범용
그래프 실행기나 학습 기능은 포함하지 않습니다.

## 생명주기

1. `GGUF파일읽기`와 `GGUF문서열기`로 파일을 통읽기합니다.
2. `Gemma4E2B모델열기`로 설정과 텐서를 검증·바인딩하고, `Gemma토크나이저열기`로 같은
   문서에서 토크나이저를 엽니다.
3. 프롬프트 길이와 최대 생성 길이의 합을 용량으로 `Gemma4실행기열기`를 호출합니다.
4. 일반 텍스트 프롬프트는 `Gemma인코드(..., 참, 거짓)`, 시스템·사용자 대화는
   `Gemma4사용자대화프롬프트`로 BOS 포함 프롬프트 ID를 만듭니다.
5. `Gemma4그리디생성` 또는 `Gemma4샘플생성`으로 새 토큰 ID를 얻고 `Gemma디코드`로
   문자열을 복원합니다. 즉시 소비하려면 `Gemma4샘플스트리밍생성`에 토큰 처리기를 넘깁니다.
6. `Gemma4실행기닫기`, `Gemma토크나이저닫기`, `GGUF소유버퍼닫기` 순으로 자원을 닫습니다.

실행기에는 세션의 K/V 캐시가 들어 있으므로 재사용해 서로 다른 프롬프트를 시작하면 안 됩니다.
동일 세션에서 직접 제어하려면 `Gemma4토큰실행`을 위치 0부터 순서대로 호출합니다.

샘플링은 `로그잇샘플러열기(Gemma로그잇개수)`로 세션 간 재사용 가능한 후보 작업공간을
준비합니다. 온도가 0 이하면 샘플러와 난수를 소비하지 않고 기존 최댓값 선택를 씁니다.
양수이면 Gemma 소프트캡 뒤 온도, top-k, top-p 순으로 적용하며 난수 시드가 같으면 같은 토큰
열을 만듭니다. 자세한 계약은 [샘플링 문서](sampler.md)에 있습니다.

## 메모리와 종료 규칙

작업공간은 약 1.27MB에 어텐션 점수 용량이 더해집니다. K/V 캐시는 공유 단위인 세 묶음마다
지역층 네 개를 512행 미러 순환 버퍼에, 전역층 하나를 전체 용량에 저장합니다. K와 V를 합친
바이트 수는 `24 × (2048 × min(용량, 512) + 512 × 용량)`입니다. 지역층의
최근 행은 순환 경계 뒤에도 연속 버퍼 뷰라 어텐션의 반복 계산 경로에서 재배열 복사가 필요 없습니다.
생성은 EOS 1, 대화 턴 106, 도구 응답 50에서 멈춥니다. 종료 ID 자체는 반환 배열에 포함됩니다.
용량에 도달하면 마지막으로 예측한 ID까지 반환하고 더 이상 정방향 계산하지 않습니다.

## 실행 예제와 검사 소스

[GemmaChat](../Sources/GemmaChat/)과 [GemmaStream](../Sources/GemmaStream/)이 대화와 스트리밍 사용 예를 제공합니다. `GemmaStream`의 모델 경로와 프롬프트는 소스에서 설정합니다. 실행 전에 지원 모델 파일을 준비하십시오.

[프로젝트 파일](../Project.hmjep)은 `GgufTest`, `TokenizerTest`, `SamplerTest`, `InferenceCompileTest` 등의 검사 산출물을 선언합니다. `RealModelTest`, `RealInferenceTest`, `RealLongContextTest`는 실제 모델 파일이 필요한 별도 검사입니다. 검사에 필요한 경로와 입력은 각 산출물의 소스를 따릅니다.

공개 저장소 루트에서 스트리밍 예제를 빌드하는 명령입니다.

```sh
hmjec --project libraries/hmje-llm --artifact GemmaStream --target posix-host-exe -o build/GemmaStream
```
