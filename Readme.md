# github repository upload

1. git status
2. git add .
3. git commit -m "Initial commit: add SSD1306 iic driver and retarget printf to USART2"
4. git branch -M main
5. git push -u origin main


3. -m 后面的双引号里是提交信息，这里写的是：
Initial commit: add SSD1306 iic driver and retarget printf to USART2

4. 把当前分支重命名为 main。
-M 表示强制重命名（如果已有 main 分支会覆盖，一般首次提交时用这个命令）。
很多老项目默认分支叫 master，现在 GitHub 等平台推荐用 main 作为默认分支名，这个命令就是用来改名字的。