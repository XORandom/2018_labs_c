/*В массиве из 10 целых чисел сосчитать сумму четных и нечетных чисел.
Вывести наибольшую их этих сумм.*/

#include <stdio.h>

int main(){
	int arr[10] = { 0 };
	int chet = 0, nechet = 0, i = 0;
	for (i = 0; i < 10; i++){
		scanf("%d", &arr[i]);
	}
	for (i = 0; i < 10; i++){
		if(arr[i]%2 == 0){
			chet = chet + arr[i];
		}
		else{
			nechet = nechet + arr[i];
		}
	}
	printf("summa chet = %d; summa nechet = %d\n", chet, nechet);
	if (chet > nechet){
		printf("naibol\'shaya: %d\n", chet);
	}
	else{
		printf("naibol\'shaya: %d\n", nechet);
	}
}
