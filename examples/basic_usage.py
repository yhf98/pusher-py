from pusher import Pusher, build_output_url, detect_protocol


def main() -> None:
    output_url = build_output_url(
        protocol="rtmp",
        host="127.0.0.1",
        app="live",
        stream="demo",
        secret="replace-me",
    )

    print("输出地址:", output_url)
    print("协议:", detect_protocol(output_url))

    pusher = Pusher(name="demo", engine="libav", log_path="pusher.log")
    print("执行命令:", " ".join(pusher.preview_command("sample.mp4", output_url)))

    # 真实推流时取消下面三行注释，并把 sample.mp4 / output_url 换成可访问的地址。
    # pusher.start("sample.mp4", output_url)
    # print(pusher.status())
    # pusher.stop()


if __name__ == "__main__":
    main()
