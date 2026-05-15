/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * File Name          : freertos.c
  * Description        : Code for freertos applications
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "oled.h"
#include "can.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */
uint8_t  g_door_status = 0;   // 车门状态: 0=关闭, 1=打开
uint16_t g_tpms_value  = 250; // 胎压数值: 默认 250 kPa
uint8_t  g_fault_code  = 0;   // 故障码: 0=正常, 1=发动机故障等

/* USER CODE END Variables */
/* Definitions for defaultTask */
osThreadId_t defaultTaskHandle;
const osThreadAttr_t defaultTask_attributes = {
  .name = "defaultTask",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for Task_Door */
osThreadId_t Task_DoorHandle;
const osThreadAttr_t Task_Door_attributes = {
  .name = "Task_Door",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityNormal,
};
/* Definitions for Task_TPMS */
osThreadId_t Task_TPMSHandle;
const osThreadAttr_t Task_TPMS_attributes = {
  .name = "Task_TPMS",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityBelowNormal,
};
/* Definitions for Task_Fault */
osThreadId_t Task_FaultHandle;
const osThreadAttr_t Task_Fault_attributes = {
  .name = "Task_Fault",
  .stack_size = 128 * 4,
  .priority = (osPriority_t) osPriorityHigh,
};
/* Definitions for Mutex_CAN_Tx */
osMutexId_t Mutex_CAN_TxHandle;
const osMutexAttr_t Mutex_CAN_Tx_attributes = {
  .name = "Mutex_CAN_Tx"
};
/* Definitions for Sem_Fault */
osSemaphoreId_t Sem_FaultHandle;
const osSemaphoreAttr_t Sem_Fault_attributes = {
  .name = "Sem_Fault"
};

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */
void Send_CAN_Msg(uint32_t stdId, uint8_t *data, uint8_t len);
/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void *argument);
void App_DoorTask(void *argument);
void App_TPMSTask(void *argument);
void App_FaultTask(void *argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */
  /* Create the mutex(es) */
  /* creation of Mutex_CAN_Tx */
  Mutex_CAN_TxHandle = osMutexNew(&Mutex_CAN_Tx_attributes);

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* Create the semaphores(s) */
  /* creation of Sem_Fault */
  Sem_FaultHandle = osSemaphoreNew(1, 0, &Sem_Fault_attributes);

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* creation of defaultTask */
  defaultTaskHandle = osThreadNew(StartDefaultTask, NULL, &defaultTask_attributes);

  /* creation of Task_Door */
  Task_DoorHandle = osThreadNew(App_DoorTask, NULL, &Task_Door_attributes);

  /* creation of Task_TPMS */
  Task_TPMSHandle = osThreadNew(App_TPMSTask, NULL, &Task_TPMS_attributes);

  /* creation of Task_Fault */
  Task_FaultHandle = osThreadNew(App_FaultTask, NULL, &Task_Fault_attributes);

  /* USER CODE BEGIN RTOS_THREADS */
  /* add threads, ... */
  /* USER CODE END RTOS_THREADS */

  /* USER CODE BEGIN RTOS_EVENTS */
  /* add events, ... */
  /* USER CODE END RTOS_EVENTS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/*
 * 函数名：  StartDefaultTask
 * 功能描述：系统默认任务（UI线程）。负责OLED屏幕的初始化，以及周期性（100ms）刷新显示全局变量中的车门状态、胎压数值和故障信息。
 * 输入参数：argument --> FreeRTOS任务入口传参，本工程中未使用
 * 输出参数：无
 * 返回值：  无 (任务函数内部为死循环，永不返回)
 */
void StartDefaultTask(void *argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  // 1. 任务启动时，只执行一次初始化和静态界面的绘制
  OLED_Init();
  OLED_Clear();
  OLED_PrintString(0, 0, "Door :");
  OLED_PrintString(0, 2, "TPMS :");
  OLED_PrintString(0, 4, "Fault:");

  /* Infinite loop */
  for(;;)
  {
    // 2. 动态刷新车门状态
    if(g_door_status == 0){
        OLED_PrintString(6, 0, "Closed "); // 多留点空格覆盖旧字符
    } 
		else{
        OLED_PrintString(6, 0, "Opened ");
    }

    // 3. 动态刷新胎压数值 (使用你驱动里的10进制打印函数)
    OLED_PrintSignedVal(6, 2, g_tpms_value);
    OLED_PrintString(10, 2, "kPa "); 

    // 4. 动态刷新故障状态
    if(g_fault_code == 0){
        OLED_PrintString(6, 4, "OK   ");
    } 
		else{
        OLED_PrintString(6, 4, "ERROR");
    }

    // 5. 延时 100ms (10FPS 的刷新率对肉眼足够了，且不占用太多CPU)
    osDelay(100);
  }
  /* USER CODE END StartDefaultTask */
}
/* USER CODE END Header_StartDefaultTask */


/* USER CODE BEGIN Header_App_DoorTask */
/*
 * 函数名：  App_DoorTask
 * 功能描述：车门状态监测任务。周期性读取车门状态（当前为翻转模拟），并调用线程安全的CAN发送接口，以 0x101 ID 周期性发送车门心跳报文。
 * 输入参数：argument --> FreeRTOS任务入口传参，本工程中未使用
 * 输出参数：无
 * 返回值：  无 (任务函数内部为死循环，永不返回)
 */
void App_DoorTask(void *argument)
{
  /* USER CODE BEGIN App_DoorTask */
  uint8_t door_data[1]; // 车门状态只需要 1 个字节
  
  /* Infinite loop */
  for(;;)
  {
    // 1. 模拟车门状态的翻转变化(演示用，比如每隔2秒开/关一次)
    // 真实场景下这里应该是读取某个 GPIO 引脚的电平
    g_door_status = !g_door_status;

    // 2. 封装数据包
    door_data[0] = g_door_status;

    // 3. 调用线程安全接口，发送车门心跳报文 (ID 设为 0x101)
    Send_CAN_Msg(0x101, door_data, 1);

    // 4. 车门数据不需要像高频传感器那样发太快，2000ms 发一次即可
    osDelay(2000);
  }
  /* USER CODE END App_DoorTask */
}
/* USER CODE END Header_App_DoorTask */

/* USER CODE BEGIN Header_App_TPMSTask */
/*
 * 函数名：  App_TPMSTask
 * 功能描述：胎压数据采集任务。周期性读取胎压数值（当前为递增模拟），将16位数据拆分为高低字节后，通过CAN总线以 0x201 ID 发送数据帧。
 * 输入参数：argument --> FreeRTOS任务入口传参，本工程中未使用
 * 输出参数：无
 * 返回值：  无 (任务函数内部为死循环，永不返回)
 */
void App_TPMSTask(void *argument)
{
  /* USER CODE BEGIN App_TPMSTask */
  uint8_t can_data[2]; // 胎压数据准备发 2 个字节
  
  /* Infinite loop */
  for(;;)
  {
    // 1. 模拟胎压数值变化 (假设全局变量 g_tpms_value 之前已经定义了)
    g_tpms_value++;
    if(g_tpms_value > 260) g_tpms_value = 240;

    // 2. 数据拆包封装 (高位在前，低位在后，方便 i.MX6ULL 侧解析)
    can_data[0] = (g_tpms_value >> 8) & 0xFF; 
    can_data[1] = g_tpms_value & 0xFF;        

    // 3. 调用刚才写的接口，通过 CAN 总线发送！(ID设为0x201)
    Send_CAN_Msg(0x201, can_data, 2);

    // 4. 睡 500ms，把 CPU 让给别人
    osDelay(500);
  }
  /* USER CODE END App_TPMSTask */
}
/* USER CODE END Header_App_TPMSTask */


/* USER CODE BEGIN Header_App_FaultTask */
/*
 * 函数名：  App_FaultTask
 * 功能描述：紧急故障处理任务（最高优先级）。平时阻塞等待二值信号量，完全让出CPU；当外部按键中断释放信号量时，瞬间苏醒抢占总线，发送 0x050 ID 的最高级报警帧。
 * 输入参数：argument --> FreeRTOS任务入口传参，本工程中未使用
 * 输出参数：无
 * 返回值：  无 (任务函数内部为死循环，永不返回)
 */
void App_FaultTask(void *argument)
{
  /* USER CODE BEGIN App_FaultTask */
  // 1. 定义一帧极其严重的故障码 (全 0xFF)
  uint8_t fault_payload[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  
  // ==================== [新增] ====================
  // 2. 定义一帧解除故障的恢复码 (全 0x00，对应网关里的 alarm_active = 0)
  uint8_t clear_payload[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  // ================================================

  /* Infinite loop */
  for(;;)
  {
    // 死等信号量：如果没有按键按下，这个任务会永远在这里沉睡
    if (osSemaphoreAcquire(Sem_FaultHandle, osWaitForever) == osOK)
    {
        // 拿到信号量，更新本地 OLED 显示 ERROR
        g_fault_code = 1; 

        // 3. 抢占总线，发送故障报文 (触发 Linux 网关变红)
        Send_CAN_Msg(0x050, fault_payload, 8);
        
        // 硬件按键消抖延时 
        osDelay(200); 
        
        // 延时 2 秒，模拟故障持续时间
        osDelay(2000);
        
        // 4. 故障恢复，本地 OLED 恢复 OK
        g_fault_code = 0;
        
        // ==================== [新增] ====================
        // 5. 通知 Linux 网关：警报解除！(触发 Linux 网关变绿)
        Send_CAN_Msg(0x050, clear_payload, 8);
        // ================================================
    }
  }
  /* USER CODE END App_FaultTask */
}
/* USER CODE END Header_App_FaultTask */

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
extern CAN_HandleTypeDef hcan;      // 引入 main.c 里的 CAN 句柄
extern osMutexId_t Mutex_CAN_TxHandle; // 引入 CubeMX 生成的互斥锁句柄

/*
 * 函数名：		Send_CAN_Msg
 * 功能描述：	线程安全的CAN报文发送接口。利用FreeRTOS互斥锁防止多任务同时抢占CAN外设，并等待硬件邮箱有空余时发送标准数据帧。
 * 输入参数：	stdId --> CAN标准帧的报文ID（如 0x101, 0x201 等）
 * 						data  --> 指向准备发送的数据数组的指针
 * 						len   --> 要发送的数据长度（DLC），范围0~8字节
 * 输出参数：	无
 * 返回值：		无
 */
void Send_CAN_Msg(uint32_t stdId, uint8_t *data, uint8_t len)
{
    CAN_TxHeaderTypeDef txHeader;
    uint32_t txMailbox;

    txHeader.StdId = stdId;           
    txHeader.ExtId = 0;								
    txHeader.IDE = CAN_ID_STD;        
    txHeader.RTR = CAN_RTR_DATA;      
    txHeader.DLC = len;               

    // 1. 获取互斥锁（最多等 10ms，拿不到就算了，绝不死等）
    if (osMutexAcquire(Mutex_CAN_TxHandle, 10) == osOK) 
    {
        // 2. 防死锁核心：如果CAN总线断开导致邮箱满了，最多等 50ms。
        // 如果 50ms 还没空位，直接丢弃这包数据，释放互斥锁，保全整个系统的运行！
        uint32_t timeout = 0;
        while(HAL_CAN_GetTxMailboxesFreeLevel(&hcan) == 0) {
            osDelay(1);
            timeout++;
            if(timeout > 50) {
                osMutexRelease(Mutex_CAN_TxHandle); // 必须先释放锁
                return; // 直接退出，丢包
            }
        }
        
        // 3. 把数据填入邮箱发送
        HAL_CAN_AddTxMessage(&hcan, &txHeader, data, &txMailbox);
        
        // 4. 发送完毕，释放互斥锁
        osMutexRelease(Mutex_CAN_TxHandle);
    }
}
/* USER CODE END Application */

