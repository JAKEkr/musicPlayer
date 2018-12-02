# musicPlayer
> simple music player with FFMPEG & SDL

 ![Build Status](https://travis-ci.org/dbader/node-datadog-metrics.svg?branch=master)



리눅스(Ubuntu 16.04)에서 Console로 실행 가능한 음악 재생 프로그램입니다.



## 설치 방법

####우분투 16.04 LTS

######라이브러리 설치

```sh
sudo add-apt-repository ppa:jonathonf/ffmpeg-4
sudo apt-get update
sudo apt-get install ffmpeg
sudo apt-get install libavfilter-dev
sudo apt-get install libpostproc-dev
sudo apt-get install libavdevice-dev
sudo apt-get install libsdl2-2.0
sudo apt-get install libsdl1.2-dev
```

###### 프로그램 설치

```
git clone https://github.com/JAKEkr/musicPlayer.git
cd musicPlayer
make
```



## 사용 예제

```
./musicPlayer (your_audio_file_name)
```

_더 많은 예제와 사용법은 [Wiki](https://github.com/JAKEkr/musicPlayer/wiki)를 참고하세요._



## 업데이트 내역

* 0.1.1
    * 수정: 문서 업데이트 (모듈 코드 동일)
* 0.1.0
    * 주석 작성
    * 의미를 쉽게 파악하기 힘들거나 불필요한 변수 재정의
    * 조건문의 함수화
* 0.0.4
    * 최신 버전의 ffmpeg에서 deprecated된 함수들 변경
* 0.0.2
    * VideoState 구조체를 audio_entry로 변경
* 0.0.1
    * https://github.com/lnmcc/musicPlayer에서 프로젝트 fork



## 라이센스

FFMPEG

- LGPL 2.1 (GNU Lesser General Public License version 2.1)
- GPL 2.0 (GNU General Public License version 2.0)

SDL

- zlib



## 정보

이종진 – https://github.com/JAKEkr

김태우 – https://github.com/Kuril951

고대훈 – https://github.com/rhgo1749

홍성임 – https://github.com/imsseong



## 기여 방법

1. (https://github.com/JAKEkr/musicPlayer/fork)을 포크합니다.
2. (`git checkout -b feat/fooBar`) 명령어로 새 브랜치를 만드세요.
3. (`git commit -am 'Add some fooBar'`) 명령어로 커밋하세요.
4. (`git push origin feat/fooBar`) 명령어로 브랜치에 푸시하세요. 
5. pull-request를 보내주세요.



## 도움을 주신 분

장문정 교수님 – https://github.com/cathmjjang