/*
 * cmd.h
 *
 *  Created on: 2026年8月5日
 *      Author: scj
 */

#ifndef INC_CMD_H_
#define INC_CMD_H_

int parse_and_dispatch(char *line);   /* 1 = 已送進 queue，0 = 本地處理完畢 */

#endif /* INC_CMD_H_ */
