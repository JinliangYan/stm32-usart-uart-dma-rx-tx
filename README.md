# STM32 使用DMA进行串口发送和接收

本文主要介绍两个主题

- 通过UART和DMA进行数据接收，支持不定字节数的数据
- 通过UART和DMA进行数据传输，避免CPU卡顿并将CPU用于其他目的

本文的应用程序指这两个主题的STM32实现代码（标准库）

## 目录

[TOC]

## 缩写

- DMA：STM32 中的直接内存访问控制器
- UART：通用异步接收器和发送器
- USART：通用同步异步接收发送器
- TX：发送
- RX：接收
- HT：半传输完成 DMA 事件/标志
- TC：传输完成 DMA 事件/标志
- RTO：接收器超时 UART 事件/标志
- IRQ：中断

## UART基本介绍

> STM32拥有USART、UART、或LPUART等外设。他们之间的差异跟本文章的目的并不重要，简而言之，USART支持在异步（UART）基础上的同步操作，而LPUART支持在STOP模式下的低功耗操作。当不使用同步模式或低功耗模式时，USART、UART和LPUART可以被视为相同的。

> 在本文章中，我将仅使用术语**UART**

STM32中的UART可以通过配置选择不同的发送（`TX`）和接收（`RX`）模式：

- 轮询模式（无DMA，无IRQ）

    - P：轮询状态位，以检查是否已传输/接收任何字符，并快速读取，以便不漏掉任何字节
    - P：易于实现，只需几行代码
    - C：在复杂应用程序中，如果CPU无法快速读取寄存器，则很容易会遗漏数据
    - C：仅适用于低波特率（<= `9600`）

- 中断模式

    - P：UART触发中断，CPU跳转到服务例程以单独处理每个接收到的字节
    - P：嵌入式应用中常用的方法
    - P：与常见波特率良好配合，`115200`，最高可达`~921600`
    - C：中断服务例程针对每个接收到的字符都会执行
    - C：如果为高速波特率触发每个字符的中断，可能会降低系统性能

- DMA模式

    - DMA用于在硬件级别将数据从USART RX数据寄存器传输到用户内存。此时不需要应用程序交互，除非需要时由应用程序处理接收到的数据
    - P：从USART外设到内存的传输在硬件级别上完成，无需CPU交互
    - P：可以很容易的与操作系统配合使用
    - P：针对最高波特率 `> 1Mbps` 和低功耗应用进行了优化
    - P：在大量数据突发的情况下，增加数据缓冲区大小可以改善功能
    - C：DMA硬件必须事先知道要传输的字节数
    - C：如果通信失败，DMA可能无法及时或完整地将已传输的字节信息通知给应用程序

    > 这篇文章只关注RX操作的DMA模式，并解释如何处理未知数据长度

所有STM32都至少有一个（`1`）UART IP和至少一个（`1`）DMA控制器。这是本文所需的全部。

实现TX的操作非常的直截了当（设置指向数据的指针，定义其长度……），但对于接收并非如此。在实现DMA接收时，应用程序需要知道在被视为 *完成* [^1]之前DMA必须处理的接收字节数。然而UART协议不提供这样的信息（它可以与更高级别的协议一起工作，本文章不涉及。我们假设我们必须实现非常可靠的低级通信协议）。

## 空闲线路（Idle Line）事件和接收器超时（Receiver Timeout）事件

STM32的UART具有检测RX线在一段时间内未活动的能力。这是通过两种方法实现的：

- Idle Line事件： 当RX线在最后接收字节后处于空闲状态[^2]1帧时间时触发。帧时间基于波特率。更高的波特率意味着单字节的帧时间更短。

- RTO（*Receiver Timeout*）事件：当线路在可编程时间内处于空闲状态时触发，完全由固件配置。

[^2]: 通常为高电平状态

这两个事件都可以触发中断，这是确保有效的接收操作的一项重要功能。

> 并非所有 STM32 都具有 IDLE LINE 或 RTO 功能。 如果不可用，则可能不会使用有关这些功能的示例。

一个例子：以`115200`波特率传输`1  byte`，大约需要`~100us`；对于`3 bytes`，总共约为`~300us`。当接收到第三个字节后，线路进入空闲状态，空闲状态达到 `1` 帧时间（在本例中为 `100us`）时，空闲线路（Idle Line）事件会触发中断。

![IDLE LINE DEMO](https://github.com/JinliangYan/JinliangYan-stm32-usart-uart-dma-rx-tx/blob/master/imgs/idle_line_demo.png)

- 应用程序接收到`3 bytes`，在 `115200 `波特率下大约需要 `300us`

- RX进入高电平状态（黄色矩形），并且UART RX 检测到它已经空闲至少`1` 帧时间（大约`100us`）
    - 黄色矩阵的宽度代表`1`帧时间
- 空闲线路（*IDLE line*）中断在绿色箭头处触发
- 在中断中，应用程序回显（echo）数据

## DMA基本介绍

STM32的DMA可以被配置为`normal`或`circular`模式。对于每种模式，DMA在触发其事件（Half-Transfer complete、Transfer-Complete）之前需要传输的元素数量。

- *Normal mode*：DMA开始数据传输，一旦传输所有元素，它就会停止并将使能位设置为0。
    - 本文例程使用这种模式发送数据
- *Circular mode*：DMA开始数据传输，一旦传输完所有元素（如写入相应长度寄存器中）它会从内存的开头重新开始并传输更多元素
    - 本文例程使用这种模式接收数据

在传输活动期间，`2`个中断可能会触发：

- *Half-Transfer complete `HT`*: 当DMA传输完一半数据时触发
- *Transfer-Complete `TC`*: 当DMA传输完所有数据时触发

> 当DMA在*circular* 模式工作时，这些中断被周期性触发

> 必须在传输开始之前将DMA硬件要传输的元素数量写入相应的DMA寄存器

### 结合UART和DMA进行数据接收

是时候理解通过使用UART和DMA的什么特性来进行数据接收从而解放CPU了，在本例中我们使用`20 bytes`的内存缓冲区数组，DMA将会把UART中接收的数据传输至该缓冲区。

下面列出了步骤（假设UART和DMA在此之前完成了基本的初始化）：

- 应用程序写入数据长度 `20` 至相关的DMA寄存器
- 应用程序写入内存和外设的地址到相关的DMA寄存器
- 应用程序将DMA方向设置为外设到内存
- 应用程序将DMA设置为 *circular* 模式。这是为了DMA在到达内存末尾是不会停止数据传输。相反，它将回到初始状态并继续将可能的更多数据从 UART 传输到内存
- 应用程序使能DMA和UART，设置为接收模式。接收不会马上开始，DMA将等待UART接收第一个字节并将它传输至数组。对于每个接收到的字节都会执行此操作。
- 应用程序收到通知——第`10`个（一半）数据被DMA从UART传输至内存时，触发`HT`事件（或中断）时
- 应用程序收到通知——第`20`个（所有）数据被DMA从UART传输至内存时，触发`TC`事件（或中断）时
- 应用程序收到通知——RX线保持空闲状态一段时间，触发UART IDLE line (或者 RTO) 事件时
- 应用程序需要处理所有这些事件才能最有效地接收

>  此配置很重要，因为我们事先不知道长度。 应用程序需要假设它可能接收到无限数量的字节，因此 DMA 必须循环运行。

> 我们使用 `20` 字节长的数组进行演示。 在实际的应用程序中，这个大小可能需要增加，这取决于 UART 波特率（速度越高，固定窗口时间中可以接收越多数据）以及应用程序处理接收到的数据的速度（使用中断通知、RTOS 或轮询模式）

### 结合UART和DMA进行数据发送

当应用程序传输数据时，由于提前知道数据长度并且准备好了要传输的内存，一切都变得更加简单。在本示例中，我们将字符串`Helloworld`作为内存方。

```c
const char
hello_world_arr[] = "HelloWorld";
```

- 应用程序写入要传输的字节数带相关的DMA寄存器，它可能是`strlen(hello_world_arr)`或者`10`
- 应用程序写入内存和外设的地址到相关的DMA寄存器
- 应用程序将DMA方向设置为内存到外设
- 应用程序将DMA设置为 *normal* 模式。一旦DMA首次完成所有数据的传输就被禁止。
- 应用程序使能DMA和UART，设置为发送模式。当 UART 接收到第一个字节并发送 DMA 请求时，DMA立即开始发送，该字节移位至 UART TX 寄存器
- 应用程序收到通知——所有字节已经通过 DMA 从 UART 传输至内存时，触发 `TC` 事件（或中断）时
- DAM停止，应用程序可能立即准备下一个发送

> 请注意，`TC` 事件是在最后一个字节通过 UART 完全发送出去**之前**触发的。这是因为 `TC ` 事件属于 DMA ，而不属于 UART 。它在 DMA 将 A 点的所有字节传输到 B 点时触发，这里 DMA 的 A 点为内存，B 点为 UART 数据寄存器。一定是`TC` 事件之后 UART 才将字节时钟输出到 GPIO 引脚

### RX下，DMA HT/TC 和 UART IDLE 组合起来的细节

这里将描述 4 种可能的情况和一种附加情况，解释为什么应用程序需要 *HC/TC* 事件

![](https://github.com/JinliangYan/JinliangYan-stm32-usart-uart-dma-rx-tx/blob/master/imgs/dma_events.svg)

这张图片用到的缩写：

- `R`：读指针，应用程序使用它从内存中读取数据。 后来也用作old_ptr
- `W`：写指针，DMA 用于写入下一个字节。 每次 DMA 写入新字节时都会增加。 后来也用作new_ptr
- `HT`：DMA 触发的半传输完成事件
- `TC`：传输完成事件 —— 由 DMA 触发
- `I`：IDLE线路事件 —— 由USART触发

DMA配置：

- Circular mode

- `20` 字节的数据长度
    - 因此，`HT` 事件在传输 10 个字节时触发
    - 因此，`TC` 事件在传输 20 个字节时触发

实际执行过程中可能出现的情况：

- Case A：DMA传输 `10` 个字节。应用程序得到 `HT` 事件的通知，可以处理接收到的数据
- Case B：DMA传输接下来的 `10` 个字节。应用程序得到 `TC` 事件的通知。可以处理从最后已知位置（`TX` 事件前 `R` 的位置）开始到内存末尾的数据。DMA处于 *circular* 模式，因此它将从缓冲区的开头继续，如图所示的顶部。

- Case C：DMA传输10字节，但不与 `HT` 事件或 `TC` 事件对齐。
    - 应用程序在传输前 `6` 字节时通过 `HT` 事件得到通知。处理可以从最后已知的读取位置开始。
    - 应用程序在接下来的 `4` 字节成功传输到内存后收到 `IDLE` 线事件。

- Case D：DMA以溢出模式传输 `10` 字节，但不与 `HT` 事件或 `TC` 事件对齐。

    - 应用程序在传输前 `4` 字节时通过 `TC` 事件得到通知。处理可以从最后已知的读取位置开始。

    - 应用程序在接下来的 `6` 字节成功传输到内存后通过 `IDLE` 事件得到通知，处理可以从缓冲区的开头开始。

- Case E：仅依赖 `IDLE` 事件时可能发生的情况的示例。
    - 如果应用程序突然接收到 `30` 字节，由于应用程序没有迅速处理，DMA将覆盖其中的10字节。
    - 一旦 `RX` 线稳定保持 `1` 字节时间，应用程序将获得 `IDLE` 线事件。
    - 数据的红色部分表示突发中最后 `10` 个字节覆盖了前 `10` 个接收到的字节。
    - 避免这种情况：
        - 更快地轮询DMA的变化以处理 `20` 字节的突发
        - 或者使用TC和HT事件

示例代码，用于从内存读取数据并对其进行处理（Case A-D）

```c
/**
 * \brief           检查使用DMA接收的新数据
 *
 * 用户必须选择从以下上下文调用此函数：
 * - 仅使用相同抢占优先级级别的中断（DMA HT，DMA TC，UART IDLE）
 * - 仅使用线程上下文（不在中断内）
 *
 * 如果从两个上下文中调用，必须实现独占访问保护。
 * 这种模式通常不建议，因为它通常意味着架构设计问题。
 *
 * 当没有IDLE中断时，应用程序必须仅依赖线程上下文，
 * 通过尽快手动调用函数，以确保从原始缓冲区读取并处理数据。
 *
 * 如果不快速进行读取，可能导致DMA溢出未读取的接收字节，
 * 因此应用程序将丢失有用的数据。
 *
 * 解决此问题的方法包括：
 * - 改进架构设计以实现更快的读取
 * - 增加原始缓冲区大小，并允许DMA在调用此函数之前写入更多数据
 */
void uart_rx_check(void) {
    /*
     * 将旧位置变量设置为静态。
     *
     * 链接器应该（在默认的C配置下）将此变量设置为`0`。
     * 它用于保持最新的读取开始位置，
     * 将此函数转换为不可重入或线程安全的函数
     */
    static size_t old_pos = 0;
    size_t pos;

    /* 计算缓冲区中的当前位置并检查是否有新数据可用 */
    pos = ARRAY_LEN(usart_rx_dma_buffer) - DMA_GetCurrDataCounter(DMA1_Channel5);
    if (pos != old_pos) {                       /* 检查接收到的数据是否发生变化 */
        if (pos > old_pos) {                    /* 当前位置位于先前位置之上 */
            /*
             * 处理以"线性"模式进行。
             *
             * 应用程序处理单个数据块时速度很快，
             * 长度通过减法计算指针来简单地计算
             *
             * [   0   ]
             * [   1   ] <- old_pos |------------------------------------|
             * [   2   ]            |                                    |
             * [   3   ]            | 单块数据 (len = pos - old_pos)      |
             * [   4   ]            |                                    |
             * [   5   ]            |------------------------------------|
             * [   6   ] <- pos
             * [   7   ]
             * [ N - 1 ]
             */
            
            /* 这里只是将数据回送串口, 可以调用其他处理函数 */
            uart_process_data(&usart_rx_dma_buffer[old_pos], pos - old_pos);
        } else {
            /*
             * 处理以"溢出"模式进行。
             *
             * 应用程序必须处理数据两次，
             * 因为有2个线性内存块要处理
             *
             * [   0   ]            |---------------------------------|
             * [   1   ]            | 第二块数据 (len = pos)         |
             * [   2   ]            |---------------------------------|
             * [   3   ] <- pos
             * [   4   ] <- old_pos |---------------------------------|
             * [   5   ]            |                                 |
             * [   6   ]            | 第一块数据 (len = N - old_pos) |
             * [   7   ]            |                                 |
             * [ N - 1 ]            |---------------------------------|
             */
            
            /* 这里只是将数据回送串口, 可以调用其他处理函数 */
            uart_process_data(&usart_rx_dma_buffer[old_pos], ARRAY_LEN(usart_rx_dma_buffer) - old_pos);
            if (pos > 0) {
                /* 这里只是将数据回送串口, 可以调用其他处理函数 */
                uart_process_data(&usart_rx_dma_buffer[0], pos);
            }
        }
        old_pos = pos;                          /* 将当前位置保存为下一次传输的旧位置 */
    }
}

```

### 中断优先级至关重要

由于Cortex-M NVIC（嵌套向量中断控制器）的灵活性，用户可以为每个NVIC中断线配置优先级。

Cortex-M中有 `2` 种优先级类型：

- 抢占优先级（Preemption priority）：具有较高逻辑优先级的中断可以抢占已经运行的较低优先级中断
- 子优先级（Subpriority）：当  `2`  （或更多）中断线同时激活时，具有较高子优先级（但抢占优先级相同）的中断将首先执行；此类中断不会停止CPU当前执行的中断（如果有）。

STM32对于DMA和UART有不同的中断线（稍后也会有中断服务例程），每个外设都有一个中断线，其优先级可以由软件配置。

被调用来处理接收到的数据的函数必须保留 *最后读取值* 的位置，因此处理函数不是线程安全的或可重入的，需要特别注意。

> 应用程序必须确保DMA和UART中断使用相同的抢占优先级。
> 这是保证处理函数永远不会被自身抢占的唯一配置（DMA中断抢占UART，或相反），否则最后已知的读取位置可能会损坏，并且应用程序将使用错误的数据进行操作。
