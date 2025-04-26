#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <unistd.h>

#define MSG_KEY 1234
#define ALERT_PIPE "/tmp/alert_pipe"
#define MAX_AMOUNT 10000

struct transaction {
    long msg_type;
    int account_id;
    double amount;
    char type[10]; // "withdrawal" or "transfer"
};

void analyze_transaction(struct transaction *trans);
void send_alert(int account_id, double amount, const char *type);

int main() {
    int msgid;
    struct transaction trans;

    // Initialize and open the message queue
    if ((msgid = msgget(MSG_KEY, 0666 | IPC_CREAT)) == -1) {
        perror("msgget");
        exit(EXIT_FAILURE);
    }

    // Continuously read messages
    while (1) {
        if (msgrcv(msgid, &trans, sizeof(struct transaction) - sizeof(long), 0, 0) == -1) {
            perror("msgrcv");
            exit(EXIT_FAILURE);
        }

        // Analyze the transaction
        analyze_transaction(&trans);
    }

    // Close the message queue (unreachable code in this example)
    if (msgctl(msgid, IPC_RMID, NULL) == -1) {
        perror("msgctl");
        exit(EXIT_FAILURE);
    }

    return 0;
}

void analyze_transaction(struct transaction *trans) {
    static int consecutive_withdrawals = 0;
    static int last_account_id = -1;

    if (strcmp(trans->type, "withdrawal") == 0) {
        if (trans->amount > MAX_AMOUNT) {
            send_alert(trans->account_id, trans->amount, trans->type);
        }

        if (trans->account_id == last_account_id) {
            consecutive_withdrawals++;
            if (consecutive_withdrawals > 3) {
                send_alert(trans->account_id, trans->amount, "consecutive withdrawals");
            }
        } else {
            consecutive_withdrawals = 1;
            last_account_id = trans->account_id;
        }
    } else if (strcmp(trans->type, "transfer") == 0) {
        // Add logic for detecting suspicious transfer patterns
    }
}

void send_alert(int account_id, double amount, const char *type) {
    FILE *pipe = fopen(ALERT_PIPE, "w");
    if (pipe == NULL) {
        perror("fopen");
        return;
    }

    fprintf(pipe, "ALERT: Account %d, Amount %.2f, Type %s\n", account_id, amount, type);
    fclose(pipe);
}
