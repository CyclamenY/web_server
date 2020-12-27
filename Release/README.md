WebServer-正式版
========
* 借鉴来源:[:fire:Linux下C++轻量级Web服务器](https://github.com/qinguoyi/TinyWebServer)

需要的东西
--------

* mysql数据库，并且已知一个足够权限的账户
* cmake与make，本项目改用cmake编译，比make文件编写要简单一些

如何编译？
--------

* 第一种方法
    - 在根目录下直接使用
        ```C++
        cmake && make
        ```

* 第二种方法
    - 创建一个新的目录，例如：`build`，并且将根目录中的`root`文件夹复制进来，在其中
        ```C++
        cmake ../ && make
        ```



## 逐步完善注释中......